// Copyright (c) 2026 The ReSatoshi developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <qt/cpuminer.h>

#include <interfaces/node.h>
#include <univalue.h>

#include <exception>
#include <limits>

namespace {
// Keep stop latency short while amortizing block-template construction.
constexpr uint32_t HASH_BATCH{1'000'000};
}

CpuMiner::CpuMiner(interfaces::Node& node) : m_node(node) {}

CpuMiner::~CpuMiner()
{
    stop();
}

bool CpuMiner::start(std::vector<std::string> destinations)
{
    if (destinations.empty() || m_running.load()) return false;
    stop();
    m_running = true;
    m_hashes = 0;
    {
        std::lock_guard lock{m_error_mutex};
        m_error.clear();
    }
    try {
        for (auto& destination : destinations) {
            m_workers.emplace_back(&CpuMiner::worker, this, std::move(destination));
        }
    } catch (...) {
        stop();
        throw;
    }
    return true;
}

void CpuMiner::stop()
{
    m_running = false;
    for (auto& worker : m_workers) {
        if (worker.joinable()) worker.join();
    }
    m_workers.clear();
}

std::string CpuMiner::error() const
{
    std::lock_guard lock{m_error_mutex};
    return m_error;
}

void CpuMiner::worker(std::string destination)
{
    uint32_t next_nonce{0};
    try {
        while (m_running.load()) {
            UniValue params{UniValue::VARR};
            params.push_back(1);
            params.push_back(destination);
            params.push_back(HASH_BATCH);
            params.push_back(next_nonce);
            const UniValue result{m_node.executeRpc("generatetoaddress", params, "")};
            // An empty result means the entire nonce batch was tested. For a
            // solved batch the exact winning nonce is not exposed, so omit it
            // rather than overstating the displayed hashrate.
            if (result.isArray() && result.empty()) {
                m_hashes += HASH_BATCH;
                next_nonce = next_nonce <= std::numeric_limits<uint32_t>::max() - 2 * HASH_BATCH
                    ? next_nonce + HASH_BATCH
                    : 0;
            }
        }
    } catch (const std::exception& e) {
        std::lock_guard lock{m_error_mutex};
        if (m_error.empty()) m_error = e.what();
        m_running = false;
    }
}
