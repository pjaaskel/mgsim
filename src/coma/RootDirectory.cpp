#include "RootDirectory.h"
#include "DDR.h"
#include "../config.h"
#include <iomanip>
using namespace std;

namespace Simulator
{

// When we shortcut a message over the ring, we want at least one slots
// available in the buffer to avoid deadlocking the ring network. This
// is not necessary for forwarding messages.
static const size_t MINSPACE_SHORTCUT = 2;
static const size_t MINSPACE_FORWARD  = 1;

COMA::RootDirectory::Line* COMA::RootDirectory::FindLine(MemAddr address, bool check_only)
{
    const MemAddr tag  = (address / m_lineSize) / m_sets;
    const size_t  set  = (size_t)((address / m_lineSize) % m_sets) * m_assoc;

    // Find the line
    Line* empty = NULL;
    for (size_t i = 0; i < m_assoc; ++i)
    {
        Line* line = &m_lines[set + i];

        if (line->state == LINE_EMPTY)
        {
            // Empty, unused line, remember this one
            empty = line;
        }
        else if (line->tag == tag)
        {
            // The wanted line was in the cache
            return line;
        }
    }

    // The line could not be found, allocate the empty line or replace an existing line
    Line* line = NULL;
    if (!check_only && empty != NULL)
    {
        // Reset the line
        line = empty;
        line->tag    = tag;
        line->state  = LINE_EMPTY;
        line->tokens = 0;
    }
    return line;
}

const COMA::RootDirectory::Line* COMA::RootDirectory::FindLine(MemAddr address) const
{
    const MemAddr tag  = (address / m_lineSize) / m_sets;
    const size_t  set  = (size_t)((address / m_lineSize) % m_sets) * m_assoc;

    // Find the line
    for (size_t i = 0; i < m_assoc; ++i)
    {
        const Line* line = &m_lines[set + i];
        if (line->state != LINE_EMPTY && line->tag == tag)
        {
            return line;
        }
    }
    return NULL;
}

bool COMA::RootDirectory::OnReadCompleted(MemAddr address, const MemData& data)
{
    assert(m_activeMsg != NULL);
    assert(m_activeMsg->address == address);
    assert(m_activeMsg->type == Message::REQUEST);
    
    COMMIT
    {
        m_activeMsg->type = Message::REQUEST_DATA_TOKEN;
        m_activeMsg->data = data;
    }
    
    if (!m_responses.Push(m_activeMsg))
    {
        DeadlockWrite("Unable to push reply into send buffer");
        return false;
    }
    
    // We're done with this request
    COMMIT{ m_activeMsg = NULL; }
    return true;
}

bool COMA::RootDirectory::OnMessageReceived(Message* msg)
{
    assert(msg != NULL);
    
    if (!p_lines.Invoke())
    {
        return false;
    }

    switch (msg->type)
    {
    case Message::REQUEST:
    {
        // Cache-line read request
        assert(msg->data.size == m_lineSize);
        
        // Find or allocate the line
        Line* line = FindLine(msg->address, false);
        if (line->state == LINE_EMPTY)
        {
            // Line has not been read yet it; queue the read
            TraceWrite(msg->address, "Received Read Request; Miss; Queuing request");
            if (!m_requests.Push(msg))
            {
                return false;
            }

            COMMIT
            {
                line->state  = LINE_LOADING;
                line->sender = msg->sender;
            }
            return true;
        }
        break;
    }
    
    case Message::REQUEST_DATA:
    {
        // We should have the line since the request already hit a copy to get the data
        Line* line = FindLine(msg->address, true);
        assert(line != NULL);
        assert(line->state == LINE_FULL);
        
        if (line->tokens > 0)
        {
            // Give the request the tokens that we have
            TraceWrite(msg->address, "Received Read Request with data; Attaching %u tokens", line->tokens);
            COMMIT
            {
                msg->type    = Message::REQUEST_DATA_TOKEN;
                msg->tokens  = line->tokens;
                line->tokens = 0;
            }
        }
        break;
    }

    case Message::EVICTION:
    {
        Line* line = FindLine(msg->address, true);
        assert(line != NULL);
        assert(line->state == LINE_FULL);
            
        unsigned int tokens = msg->tokens + line->tokens;
        assert(tokens <= m_numCaches);
        
        if (tokens < m_numCaches)
        {
            // We don't have all the tokens, so just store the new token count
            TraceWrite(msg->address, "Received Evict Request; Adding its %u tokens to directory's %u tokens", msg->tokens, line->tokens);
            COMMIT
            {
                line->tokens = tokens;
                delete msg;
            }
        }
        else
        {
            // Evict message with all tokens, discard and remove the line from the system.
            if (msg->dirty)
            {
                TraceWrite(msg->address, "Received Evict Request; All tokens; Writing back and clearing line from system");
                
                // Line has been modified, queue the writeback
                if (!m_requests.Push(msg))
                {
                    return false;
                }
            }
            else
            {
                TraceWrite(msg->address, "Received Evict Request; All tokens; Clearing line from system");
                COMMIT{ delete msg; }
            }            
            COMMIT{ line->state = LINE_EMPTY; }
        }
        return true;
    }
    
    case Message::UPDATE:
    case Message::REQUEST_DATA_TOKEN:
        // Just forward it
        break;
                    
    default:
        assert(false);
        break;
    }

    // Forward the request
    if (!SendMessage(msg, MINSPACE_SHORTCUT))
    {
        // Can't shortcut the message, go the long way
        COMMIT{ msg->ignore = true; }
        if (!m_requests.Push(msg))
        {
            DeadlockWrite("Unable to forward request");
            return false;
        }
    }
    return true;
}

Result COMA::RootDirectory::DoIncoming()
{
    // Handle incoming message from previous node
    assert(!m_incoming.Empty());
    if (!OnMessageReceived(m_incoming.Front()))
    {
        return FAILED;
    }
    m_incoming.Pop();
    return SUCCESS;
}

Result COMA::RootDirectory::DoRequests()
{
    assert(!m_requests.Empty());

    if (m_activeMsg != NULL)
    {
        // We're currently processing a read that will produce a reply, stall
        return FAILED;
    }
    
    Message* msg = m_requests.Front();
    if (msg->ignore)
    {
        // Ignore this message; put on responses queue for re-insertion into global ring
        if (!m_responses.Push(msg))
        {
            return FAILED;
        }
    }
    else
    {
        if (msg->type == Message::REQUEST)
        {
            // It's a read
            if (!m_memory->Read(msg->address, m_lineSize))
            {
                return FAILED;
            }
            COMMIT{ m_activeMsg = msg; }
        }
        else
        {
            // It's a write
            assert(msg->type == Message::EVICTION);
            if (!m_memory->Write(msg->address, msg->data.data, msg->data.size))
            {
                return FAILED;
            }
            COMMIT{ delete msg; }
        }
    }
    m_requests.Pop();
    return SUCCESS;
}

Result COMA::RootDirectory::DoResponses()
{
    assert(!m_responses.Empty());
    Message* msg = m_responses.Front();

    // We need this arbitrator for the output channel anyway,
    // even if we don't need or modify any line.
    if (!p_lines.Invoke())
    {
        return FAILED;
    }

    if (!msg->ignore)
    {
        // We should have a loading line for this
        Line* line = FindLine(msg->address, true);
        assert(line != NULL);
        assert(line->state == LINE_LOADING);

        TraceWrite(msg->address, "Sending Read Response with %u tokens", (unsigned)m_numCaches);

        COMMIT
        {
            // Since this comes from memory, the reply has all tokens
            msg->tokens = m_numCaches;
            msg->sender = line->sender;
    
            // The line has now been read
            line->state = LINE_FULL;
        }
    }
    
    COMMIT{ msg->ignore = false; }
    
    if (!SendMessage(msg, MINSPACE_FORWARD))
    {
        return FAILED;
    }
    
    m_responses.Pop();
    return SUCCESS;
}

COMA::RootDirectory::RootDirectory(const std::string& name, COMA& parent, VirtualMemory& memory, size_t numCaches, const Config& config) :
    Simulator::Object(name, parent),
    COMA::Object(name, parent),
    DirectoryBottom(name, parent),
    m_lineSize(config.getInteger<size_t>("CacheLineSize",           64)),
    m_assoc   (config.getInteger<size_t>("COMACacheAssociativity",   4) * numCaches),
    m_sets    (config.getInteger<size_t>("COMACacheNumSets",       128)),
    m_numCaches(numCaches),
    p_lines    (*this, "p_lines"),    
    m_requests (*parent.GetKernel(), INFINITE),
    m_responses(*parent.GetKernel(), INFINITE),
    m_activeMsg(NULL),
    p_Incoming ("incoming",  delegate::create<RootDirectory, &RootDirectory::DoIncoming>(*this)),
    p_Requests ("requests",  delegate::create<RootDirectory, &RootDirectory::DoRequests>(*this)),
    p_Responses("responses", delegate::create<RootDirectory, &RootDirectory::DoResponses>(*this))
{
    assert(m_lineSize <= MAX_MEMORY_OPERATION_SIZE);
    
    // Create the cache lines
    // We need as many cache lines in the directory to cover all caches below it
    m_lines.resize(m_assoc * m_sets);
    for (size_t i = 0; i < m_lines.size(); ++i)
    {
        m_lines[i].state = LINE_EMPTY;
    }

    m_incoming.Sensitive(p_Incoming);
    m_requests.Sensitive(p_Requests);
    m_responses.Sensitive(p_Responses);
    
    p_lines.AddProcess(p_Incoming);
    p_lines.AddProcess(p_Responses);

    m_memory = new DDRChannel("ddr", *this, memory);
}

COMA::RootDirectory::~RootDirectory()
{
    delete m_memory;
}

void COMA::RootDirectory::Cmd_Help(std::ostream& out, const std::vector<std::string>& arguments) const
{
    out <<
    "The Root Directory in a COMA system is connected via other nodes in the COMA\n"
    "system via a ring network. It acts as memory controller for a DDR channel which\n"
    "serves as the backing store.\n\n"
    "Supported operations:\n"
    "- read <component>\n"
    "  Reads and displays the directory lines, and global information such as hit-rate\n"
    "  and directory configuration.\n"
    "- read <component> buffers\n"
    "  Reads and displays the buffers in the directory\n";
}

void COMA::RootDirectory::Cmd_Read(std::ostream& out, const std::vector<std::string>& arguments) const
{
    if (!arguments.empty() && arguments[0] == "buffers")
    {
        // Print the buffers
        Print(out);
        return;
    }

    out << "Cache type:          ";
    if (m_assoc == 1) {
        out << "Direct mapped" << endl;
    } else if (m_assoc == m_lines.size()) {
        out << "Fully associative" << endl;
    } else {
        out << dec << m_assoc << "-way set associative" << endl;
    }
    out << endl;

    // No more than 4 columns per row and at most 1 set per row
    const size_t width = std::min<size_t>(m_assoc, 4);

    out << "Set |";
    for (size_t i = 0; i < width; ++i) out << "        Address       |";
    out << endl << "----";
    std::string seperator = "+";
    for (size_t i = 0; i < width; ++i) seperator += "----------------------+";
    out << seperator << endl;

    for (size_t i = 0; i < m_lines.size() / width; ++i)
    {
        const size_t index = (i * width);
        const size_t set   = index / m_assoc;

        if (index % m_assoc == 0) {
            out << setw(3) << setfill(' ') << dec << right << set;
        } else {
            out << "   ";
        }

        out << " | ";
        for (size_t j = 0; j < width; ++j)
        {
            const Line& line = m_lines[index + j];
            if (line.state == LINE_EMPTY) {
                out << "                    ";
            } else {
                out << hex << "0x" << setw(16) << setfill('0') << (line.tag * m_sets + set) * m_lineSize;
                if (line.state == LINE_LOADING) {
                    out << " L";
                } else {
                    out << "  ";
                }
            }
            out << " | ";
        }
        out << endl
            << ((index + width) % m_assoc == 0 ? "----" : "    ")
            << seperator << endl;
    }
}

}