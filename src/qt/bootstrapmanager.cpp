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
#include <netgroup.h>
#include <util/strencodings.h>
#include <util/string.h>

#include <QRegularExpression>
#include <QSettings>
#include <QStringList>

#include <algorithm>
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
    // An inbound-only remainder must not suppress bootstrap recovery.
    if (std::none_of(peers.begin(), peers.end(), [](const Peer& peer) { return peer.bootstrap || peer.ready; })) {
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
        if (now - since >= std::chrono::seconds{120}) ++stable;
    }
    if (stable >= 3) {
        action.release = true;
        for (const auto& peer : peers) {
            if (peer.bootstrap) action.disconnect.push_back(peer.id);
        }
    }
    return action;
}

BootstrapManager::BootstrapManager(interfaces::Node& node, Connector connector, bool persist)
    : m_persist(persist), m_node(node), m_connector(std::move(connector))
{
    if (!m_connector) m_connector = [&node](const std::string& name, bool add) {
        // Core owns DNS, sockets and retries. Only its connection list changes.
        return add ? node.addNode(name) : node.removeAddedNode(name);
    };
    if (m_persist) {
        const auto saved = QSettings{}.value("recoveryPeers/mainnet").toStringList();
        for (const auto& address : saved) {
            auto normalized = normalizeAddress(address.toStdString());
            if (normalized && m_recovery.size() < 32 && std::find(m_recovery.begin(), m_recovery.end(), *normalized) == m_recovery.end()) m_recovery.push_back(*normalized);
        }
    }
}

BootstrapManager::~BootstrapManager()
{
    for (const auto& address : m_recovery) m_node.cancelOneTry(address);
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

bool BootstrapPolicy::retryRecovery(Clock::time_point now, size_t connected, bool enabled)
{
    if (!enabled || connected != 0) {
        m_zero_since.reset();
        m_last_recovery_retry.reset();
        return false;
    }
    if (!m_zero_since) m_zero_since = now;
    if (now - *m_zero_since < std::chrono::seconds{60} ||
        (m_last_recovery_retry && now - *m_last_recovery_retry < std::chrono::seconds{60})) return false;
    m_last_recovery_retry = now;
    return true;
}

std::optional<std::string> BootstrapManager::normalizeAddress(const std::string& input)
{
    const QString text = QString::fromStdString(input).trimmed();
    if (text.isEmpty() || text.size() > 300 || text.contains(QRegularExpression{"[\\s/@?#%]"}) || text.contains("://")) return {};
    uint16_t port{19333};
    std::string host;
    if (!SplitHostPort(text.toStdString(), port, host) || port == 0 || host.empty()) return {};
    const auto numeric = LookupNumeric(host, port);
    if (numeric.IsValid()) return numeric.ToStringAddrPort();
    QString domain = QString::fromStdString(host).toLower();
    if (domain.endsWith('.')) domain.chop(1);
    if (domain.size() > 253 || !domain.contains('.') || !domain.contains(QRegularExpression{"[a-z]"})) return {};
    for (const auto& label : domain.split('.')) {
        if (!QRegularExpression{"^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?$"}.match(label).hasMatch()) return {};
    }
    return domain.toStdString() + ":" + util::ToString(port);
}

bool BootstrapManager::saveSettings()
{
    if (!m_persist) return true;
    QStringList saved;
    for (const auto& address : m_recovery) saved.push_back(QString::fromStdString(address));
    QSettings settings;
    settings.setValue("recoveryPeers/mainnet", saved);
    settings.sync();
    return settings.status() == QSettings::NoError;
}

bool BootstrapManager::saveAddress(const std::string& input)
{
    auto address = normalizeAddress(input);
    if (!address || m_recovery.size() >= 32 || std::find(m_recovery.begin(), m_recovery.end(), *address) != m_recovery.end()) return false;
    m_recovery.push_back(*address);
    if (!saveSettings()) { m_recovery.pop_back(); return false; }
    return true;
}

bool BootstrapManager::removeAddress(const std::string& address)
{
    const auto previous = m_recovery;
    std::erase(m_recovery, address);
    if (!saveSettings()) { m_recovery = previous; return false; }
    m_node.cancelOneTry(address);
    interfaces::Node::NodesStats stats;
    if (m_node.getNodesStats(stats)) for (const auto& item : stats) {
        if (std::get<0>(item).m_addr_name == address) m_node.disconnectById(std::get<0>(item).nodeid);
    }
    return true;
}

bool BootstrapManager::connectAddress(const std::string& address)
{
    if (!m_node.getNetworkActive() || gArgs.IsArgSet("-connect") || std::find(m_recovery.begin(), m_recovery.end(), address) == m_recovery.end()) return false;
    const auto status = m_node.oneTryStatus(address);
    if (status == CConnman::OneTryStatus::CONNECTED || status == CConnman::OneTryStatus::CONNECTING) return true;
    return m_node.connectOneTry(address);
}

CConnman::OneTryStatus BootstrapManager::recoveryStatus(const std::string& address) const
{
    return m_node.oneTryStatus(address);
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
        m_policy.retryRecovery(now, 0, false);
        for (const auto& address : m_recovery) m_node.cancelOneTry(address);
        release();
        m_started = false;
        return;
    }
    const bool resolved{m_node.getSeedAddresses(m_addresses) || HaveNameProxy()};
    std::vector<std::string> seeds;
    for (auto seed : Params().DNSSeeds()) {
        if (seed.ends_with('.')) seed.pop_back();
        seeds.push_back(seed + ":" + util::ToString(Params().GetDefaultPort()));
    }
    interfaces::Node::NodesStats stats;
    if (!m_node.getNodesStats(stats)) return;
    // Missing statistics under cs_main contention are not evidence of a bad
    // peer. Skip this observation without resetting its stability interval.
    for (const auto& item : stats) {
        if (m_node.isConnected(std::get<0>(item).nodeid) && !std::get<1>(item)) return;
    }
    // Use only the current, bounded DNS snapshots. Active bootstrap NodeIds
    // remain excluded even after DNS changes or the snapshot expires.
    std::set<CService> recovery_endpoints;
    for (const auto& address : m_recovery) {
        const auto resolved_addresses = m_node.oneTryAddresses(address);
        recovery_endpoints.insert(resolved_addresses.begin(), resolved_addresses.end());
        auto numeric = LookupNumeric(address, 19333);
        if (numeric.IsValid()) recovery_endpoints.insert(numeric);
        for (const auto& item : stats) {
            if (std::get<0>(item).m_addr_name == address) recovery_endpoints.insert(std::get<0>(item).addr);
        }
    }
    size_t connected{0};
    for (const auto& item : stats) if (m_node.isConnected(std::get<0>(item).nodeid)) ++connected;
    if (m_policy.retryRecovery(now, connected, active)) {
        for (const auto& address : m_recovery) connectAddress(address);
    }
    std::vector<BootstrapPolicy::Peer> peers;
    std::set<int64_t> present;
    const auto groups = NetGroupManager::NoAsmap();
    std::set<std::vector<unsigned char>> ready_groups;
    for (const auto& item : stats) {
        const auto& peer{std::get<0>(item)};
        present.insert(peer.nodeid);
        bool bootstrap{m_bootstrap_ids.contains(peer.nodeid) || recovery_endpoints.contains(peer.addr) ||
                       std::find(m_recovery.begin(), m_recovery.end(), peer.m_addr_name) != m_recovery.end() ||
                       (peer.addr.GetPort() == Params().GetDefaultPort() && m_addresses.contains(peer.addr))};
        for (const auto& seed : seeds) {
            auto dotted{seed};
            dotted.insert(dotted.rfind(':'), ".");
            bootstrap |= peer.m_addr_name == seed || peer.m_addr_name == dotted;
        }
        if (bootstrap) m_bootstrap_ids.insert(peer.nodeid);
        // Inbound connections and user-supplied addnodes must not manufacture
        // the independent replacements that retire our bootstrap connections.
        const bool useful{peer.m_conn_type == ConnectionType::OUTBOUND_FULL_RELAY ||
                          peer.m_conn_type == ConnectionType::BLOCK_RELAY};
        const bool serves_blocks{std::get<1>(item) &&
            (std::get<2>(item).their_services & (NODE_NETWORK | NODE_NETWORK_LIMITED)) != 0};
        const bool ready{(resolved || !m_recovery.empty()) && !bootstrap && useful && serves_blocks &&
            m_node.isConnected(peer.nodeid) && peer.addr.IsRoutable() &&
            ready_groups.insert(groups.GetGroup(peer.addr)).second};
        peers.push_back({peer.nodeid, bootstrap, ready});
    }
    std::erase_if(m_bootstrap_ids, [&](int64_t id) { return !present.contains(id); });
    int headers{0};
    int64_t header_time{0};
    const bool synchronized{!m_node.isLoadingBlocks() && !m_node.isInitialBlockDownload() &&
        m_node.getHeaderTip(headers, header_time) && headers <= m_node.getNumBlocks()};
    const auto action{m_policy.update(now, peers, active, synchronized)};
    if (action.release) {
        release();
        for (const auto& address : m_recovery) m_node.cancelOneTry(address);
    }
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
