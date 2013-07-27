#ifndef CDMA_ROOTDIRECTORY_H
#define CDMA_ROOTDIRECTORY_H

#include "Directory.h"
#include <arch/mem/DDR.h>

#include <queue>
#include <set>

class Config;

namespace Simulator
{

class DDRChannel;
class DDRChannelRegistry;

class CDMA::RootDirectory : public CDMA::DirectoryBottom, public DDRChannel::ICallback, public Inspect::Interface<Inspect::Read>
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
        unsigned int tokens;   ///< Full: tokens stored here by evictions
        NodeID       sender;   ///< Loading: ID of the cache that requested the loading line
    };

private:
    std::map<MemAddr, Line> m_dir;///< The cache lines
    size_t            m_maxNumLines;///< Maximum number of lines in this directory
    size_t            m_lineSize;   ///< The size of a cache-line
    size_t            m_id;         ///< Which root directory we are (0 <= m_id < m_numRoots)
    size_t            m_numRoots;   ///< Number of root directories on the top-level ring

    ArbitratedService<CyclicArbitratedPort> p_lines;      ///< Arbitrator for lines and output

    DDRChannel*       m_memory;    ///< DDR memory channel
    Buffer<Message*>  m_requests;  ///< Requests to memory
    Buffer<Message*>  m_responses; ///< Responses from memory
    std::queue<Message*> m_active;  ///< Messages active in DDR

    // Processes
    Process p_Incoming;
    Process p_Requests;
    Process p_Responses;

    bool  IsLocalAddress(MemAddr address) const;
    Line* FindLine(MemAddr address);
    Line* AllocateLine(MemAddr address);
    bool  OnMessageReceived(Message* msg);
    bool  OnReadCompleted();

    // Processes
    Result DoIncoming();
    Result DoRequests();
    Result DoResponses();

    // Statistics
    uint64_t          m_nreads;
    uint64_t          m_nwrites;

    // Administrative
    friend class CDMA;

public:
    RootDirectory(const std::string& name, CDMA& parent, Clock& clock, size_t id, const DDRChannelRegistry& ddr, Config& config);
    RootDirectory(const RootDirectory&) = delete;
    RootDirectory& operator=(const RootDirectory&) = delete;

    // Updates the internal data structures
    void Initialize();

    // Administrative
    void Cmd_Info(std::ostream& out, const std::vector<std::string>& arguments) const;
    void Cmd_Read(std::ostream& out, const std::vector<std::string>& arguments) const;


    // Statistics
    void GetMemoryStatistics(uint64_t& nreads_ext, uint64_t& nwrites_ext) const
    {
        nreads_ext = m_nreads;
        nwrites_ext = m_nwrites;
    }
};

}
#endif
