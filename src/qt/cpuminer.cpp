// Copyright (c) 2026 The ReSatoshi developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <qt/cpuminer.h>
#include <qt/cpuminerhash.h>

#include <arith_uint256.h>
#include <chainparams.h>
#include <consensus/merkle.h>
#include <interfaces/mining.h>
#include <interfaces/node.h>
#include <key_io.h>
#include <script/solver.h>
#include <univalue.h>

#include <chrono>
#include <exception>
#include <limits>
#include <set>
#include <stdexcept>

CpuMiner::CpuMiner(interfaces::Node& node) : m_node(node) {}
CpuMiner::~CpuMiner() { stop(); }

bool CpuMiner::start(std::vector<std::string> destinations)
{
    if (destinations.empty() || running() || stopping()) return false;
    if (std::set<std::string>(destinations.begin(), destinations.end()).size() != destinations.size()) return false;
    stop();
    m_running = true;
    m_hashes = 0;
    m_paused = false;
    {
        std::lock_guard lock{m_error_mutex};
        m_error.clear();
    }
    try {
        for (auto& destination : destinations) {
            ++m_active;
            try {
                m_workers.emplace_back(&CpuMiner::worker, this, std::move(destination));
            } catch (...) {
                --m_active;
                throw;
            }
        }
    } catch (...) {
        stop();
        throw;
    }
    return true;
}

void CpuMiner::requestStop() { m_running = false; }

void CpuMiner::stop()
{
    requestStop();
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
    using Clock = std::chrono::steady_clock;
    uint64_t pending_hashes{0};
    try {
        const auto decoded{DecodeDestination(destination)};
        if (!IsValidDestination(decoded)) throw std::runtime_error("Invalid address");
        const auto script{GetScriptForDestination(decoded)};
        auto mining{m_node.makeMining()};
        const bool regtest{Params().GetChainType() == ChainType::REGTEST};
        const auto ready = [&] {
            if (m_pause_requested.load() || m_node.shutdownRequested()) return false;
            if (regtest) return true;
            return m_node.isReadyToMine();
        };
        while (running() && !m_node.shutdownRequested()) {
            if (!ready()) {
                m_paused = true;
                std::this_thread::sleep_for(std::chrono::milliseconds{20});
                continue;
            }
            m_paused = false;
            auto candidate{mining->createNewBlock({.coinbase_output_script = script, .include_dummy_extranonce = true}, /*cooldown=*/false)};
            if (!candidate || !running()) break;
            CBlock block{candidate->getBlock()};
            CMutableTransaction coinbase{*block.vtx[0]};
            // Unique across workers, template refreshes and stop/start cycles.
            // Changing only scriptSig preserves the witness commitment.
            coinbase.vin[0].scriptSig << static_cast<int64_t>(++m_template_id);
            block.vtx[0] = MakeTransactionRef(std::move(coinbase));
            block.hashMerkleRoot = BlockMerkleRoot(block);
            CBlockHeader header{block};
            CpuMinerHasher hasher{header};
            bool negative{false}, overflow{false};
            arith_uint256 target;
            target.SetCompact(header.nBits, &negative, &overflow);
            if (negative || overflow || target == 0) throw std::runtime_error("Invalid mining target");
            const auto refresh{Clock::now() + std::chrono::seconds{1}};
            for (uint64_t nonce{0}; nonce <= std::numeric_limits<uint32_t>::max(); ++nonce) {
                if (!running() || m_pause_requested.load()) break;
                if ((nonce & 4095) == 0) {
                    m_hashes += pending_hashes;
                    pending_hashes = 0;
                    if (!ready() || m_node.getBestBlockHash() != header.hashPrevBlock || Clock::now() >= refresh) break;
                }
                header.nNonce = static_cast<uint32_t>(nonce);
                ++pending_hashes;
                if (UintToArith256(hasher.hash(header.nNonce)) <= target) {
                    if (running() && ready() && m_node.getBestBlockHash() == header.hashPrevBlock) {
                        if (!candidate->submitSolution(header.nVersion, header.nTime, header.nNonce, block.vtx[0])) {
                            throw std::runtime_error("Mined block was not accepted");
                        }
                    }
                    break;
                }
            }
            m_hashes += pending_hashes;
            pending_hashes = 0;
        }
    } catch (const UniValue& e) {
        std::lock_guard lock{m_error_mutex};
        if (m_error.empty()) {
            const UniValue& message{e.find_value("message")};
            m_error = message.isStr() ? message.get_str() : e.write();
        }
        requestStop();
    } catch (const std::exception& e) {
        std::lock_guard lock{m_error_mutex};
        if (m_error.empty()) m_error = e.what();
        requestStop();
    }
    m_hashes += pending_hashes;
    if (--m_active == 0) requestStop();
}
