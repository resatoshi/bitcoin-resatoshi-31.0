// Copyright (c) 2026 The ReSatoshi developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_QT_CPUMINER_H
#define BITCOIN_QT_CPUMINER_H

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace interfaces { class Node; }

/** Small in-process CPU miner used by the desktop dashboard.
 *
 * Every worker uses a different wallet-owned destination, so workers do not
 * hash identical block headers. RPC never leaves the process.
 */
class CpuMiner
{
public:
    explicit CpuMiner(interfaces::Node& node);
    ~CpuMiner();

    CpuMiner(const CpuMiner&) = delete;
    CpuMiner& operator=(const CpuMiner&) = delete;

    bool start(std::vector<std::string> destinations);
    void stop();
    bool running() const { return m_running.load(); }
    uint64_t hashes() const { return m_hashes.load(); }
    size_t threadCount() const { return m_workers.size(); }
    std::string error() const;

private:
    void worker(std::string destination);

    interfaces::Node& m_node;
    std::atomic<bool> m_running{false};
    std::atomic<uint64_t> m_hashes{0};
    std::vector<std::thread> m_workers;
    mutable std::mutex m_error_mutex;
    std::string m_error;
};

#endif // BITCOIN_QT_CPUMINER_H
