// Copyright (c) 2026 The ReSatoshi developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <qt/bootstrapmanager.h>

#include <chainparams.h>
#include <common/args.h>
#include <interfaces/node.h>
#include <logging.h>
#include <net.h>
#include <net_processing.h>
#include <netbase.h>

#include <exception>

BootstrapPolicy::Action BootstrapPolicy::update(Clock::time_point now, const std::vector<Peer>& peers, bool network_active, bool synchronized)
{
    Action action;
    if (!network_active) {
        m_ready_since.clear();
        m_last_retry.reset();
        m_had_peers = false;
        return action;
    }
    if (peers.empty()) {
        m_ready_since.clear();
        if (m_had_peers || !m_last_retry || now - *m_last_retry >= std::chrono::seconds{60}) {
            action.reconnect = true;
            m_last_retry = now;
        }
        m_had_peers = false;
        return action;
    }
    m_had_peers = true;
    std::set<int64_t> ready;
    for (const auto& peer : peers) {
        if (synchronized && peer.ready && !peer.bootstrap) {
            ready.insert(peer.id);
            m_ready_since.try_emplace(peer.id, now);
        }
    }
    std::erase_if(m_ready_since, [&](const auto& entry) { return !ready.contains(entry.first); });
    size_t stable{0};
    for (const auto& [id, since] : m_ready_since) {
        if (now - since >= std::chrono::seconds{60}) ++stable;
    }
    if (stable >= 3) {
        action.release = true;
        for (const auto& peer : peers) {
            if (peer.bootstrap) action.disconnect.push_back(peer.id);
        }
    }
    return action;
}

BootstrapManager::BootstrapManager(interfaces::Node& node, Connector connector)
    : m_node(node), m_connector(std::move(connector))
{
    if (!m_connector) m_connector = [&node](const std::string& name, bool add) {
        // Core owns DNS, sockets and retries. Only its connection list changes.
        return add ? node.addNode(name) : node.removeAddedNode(name);
    };
}

BootstrapManager::~BootstrapManager()
{
    release();
}

void BootstrapManager::release()
{
    // Only unregister entries added by this manager, preserving user addnodes.
    for (const auto& seed : m_owned_seeds) {
        try { m_connector(seed, false); } catch (const std::exception&) {}
    }
    m_owned_seeds.clear();
}

void BootstrapManager::poll(Clock::time_point now)
{
    if (now < m_next_poll) return;
    m_next_poll = now + std::chrono::seconds{1};
    if (m_node.shutdownRequested()) return;
    const bool active{m_node.getNetworkActive()};
    // Explicit connection-only configurations take precedence over discovery.
    if (!active || gArgs.IsArgSet("-connect")) {
        m_policy.update(now, {}, false);
        release();
        m_started = false;
        return;
    }
    const bool resolved{m_node.getSeedAddresses(m_addresses) || HaveNameProxy()};
    std::vector<std::string> seeds;
    for (auto seed : Params().DNSSeeds()) {
        if (seed.ends_with('.')) seed.pop_back();
        seeds.push_back(seed + ":" + std::to_string(Params().GetDefaultPort()));
    }
    interfaces::Node::NodesStats stats;
    if (!m_node.getNodesStats(stats)) return;
    // Missing statistics under cs_main contention are not evidence of a bad
    // peer. Skip this observation without resetting its stability interval.
    for (const auto& item : stats) {
        if (m_node.isConnected(std::get<0>(item).nodeid) && !std::get<1>(item)) return;
    }
    std::vector<BootstrapPolicy::Peer> peers;
    std::set<int64_t> present;
    for (const auto& item : stats) {
        const auto& peer{std::get<0>(item)};
        present.insert(peer.nodeid);
        bool bootstrap{m_bootstrap_ids.contains(peer.nodeid) ||
                       (peer.addr.GetPort() == Params().GetDefaultPort() && m_addresses.contains(peer.addr))};
        for (const auto& seed : seeds) {
            auto dotted{seed};
            dotted.insert(dotted.rfind(':'), ".");
            bootstrap |= peer.m_addr_name == seed || peer.m_addr_name == dotted;
        }
        if (bootstrap) m_bootstrap_ids.insert(peer.nodeid);
        const bool useful{peer.m_conn_type != ConnectionType::FEELER &&
                          peer.m_conn_type != ConnectionType::ADDR_FETCH &&
                          peer.m_conn_type != ConnectionType::PRIVATE_BROADCAST};
        const bool serves_blocks{std::get<1>(item) &&
            (std::get<2>(item).their_services & (NODE_NETWORK | NODE_NETWORK_LIMITED)) != 0};
        peers.push_back({peer.nodeid, bootstrap, resolved && useful && serves_blocks && m_node.isConnected(peer.nodeid)});
    }
    std::erase_if(m_bootstrap_ids, [&](int64_t id) { return !present.contains(id); });
    int headers{0};
    int64_t header_time{0};
    const bool synchronized{!m_node.isLoadingBlocks() && !m_node.isInitialBlockDownload() &&
        m_node.getHeaderTip(headers, header_time) && headers <= m_node.getNumBlocks()};
    const auto action{m_policy.update(now, peers, active, synchronized)};
    if (action.release) release();
    for (const auto id : action.disconnect) {
        size_t remaining{0};
        for (const auto& peer : peers) {
            if (peer.ready && !peer.bootstrap && m_node.isConnected(peer.id)) ++remaining;
        }
        if (remaining < 3) break;
        if (m_node.disconnectById(id)) LogInfo("CPU miner: disconnected bootstrap peer %d after stable peer handoff\n", id);
    }
    if ((!m_started || action.reconnect) && !action.release) {
        for (const auto& seed : seeds) {
            if (!m_owned_seeds.contains(seed) && m_connector(seed, true)) m_owned_seeds.insert(seed);
        }
        m_started = true;
    }
}
