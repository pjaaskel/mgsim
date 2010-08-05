#ifndef COMA_ROOTDIRECTORY_H
#define COMA_ROOTDIRECTORY_H

#include "Directory.h"
#include "DDR.h"
#include <queue>
#include <set>

class Config;

namespace Simulator
{

class DDRChannel;

class COMA::RootDirectory : public COMA::DirectoryBottom, public DDRChannel::ICallback
{
public:    
    enum LineState
    {
        LINE_EMPTY,
        LINE_LOADING,
        LINE_FULL
    };
    
    struct Line
    {
        LineState    state;    ///< State of the line
        MemAddr      tag;      ///< Tag of this line
        unsigned int tokens;   ///< Full: tokens stored here by evictions
        CacheID      sender;   ///< Loading: ID of the cache that requested the loading line
    };

private:
    std::vector<Line> m_lines;      ///< The cache lines
    size_t            m_lineSize;   ///< The size of a cache-line
    size_t            m_assoc;      ///< Number of lines in a set
    size_t            m_sets;       ///< Number of sets
    size_t            m_numCaches;
    
    ArbitratedService<> p_lines;      ///< Arbitrator for lines and output
    
    DDRChannel*       m_memory;    ///< DDR memory channel
    Buffer<Message*>  m_requests;  ///< Requests to memory
    Buffer<Message*>  m_responses; ///< Responses from memory
    Message*          m_activeMsg; ///< Currently active message to the memory
    
    // Processes
    Process p_Incoming;
    Process p_Requests;
    Process p_Responses;
    
    Line* FindLine(MemAddr address, bool check_only);
    bool  OnMessageReceived(Message* msg);
    bool  OnReadCompleted(MemAddr address, const MemData& data);
    
    // Processes
    Result DoIncoming();
    Result DoRequests();
    Result DoResponses();

public:
    RootDirectory(const std::string& name, COMA& parent, VirtualMemory& memory, size_t numCaches, const Config& config);
    ~RootDirectory();
    
    // Administrative
    const Line* FindLine(MemAddr address) const;

    void Cmd_Help(std::ostream& out, const std::vector<std::string>& arguments) const;
    void Cmd_Read(std::ostream& out, const std::vector<std::string>& arguments) const;
};

}
#endif