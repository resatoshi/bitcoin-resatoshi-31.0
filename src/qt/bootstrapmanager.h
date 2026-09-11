// Copyright (c) 2026 The ReSatoshi developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_QT_BOOTSTRAPMANAGER_H
#define BITCOIN_QT_BOOTSTRAPMANAGER_H

#include <netaddress.h>
#include <net.h>

#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace interfaces { class Node; }

/** Monotonic-time policy; each replacement peer must itself stay ready for 120s. */
class BootstrapPolicy
{
public:
    using Clock = std::chrono::steady_clock;
    struct Peer { int64_t id; bool bootstrap; bool ready; };
    struct Action { std::vector<int64_t> disconnect; bool reconnect{false}; bool release{false}; };
    Action update(Clock::time_point now, const std::vector<Peer>& peers, bool network_active, bool synchronized = true);
    bool retryRecovery(Clock::time_point now, size_t connected, bool enabled);

private:
    std::map<int64_t, Clock::time_point> m_ready_since;
    std::optional<Clock::time_point> m_last_retry;
    bool m_had_peers{false};
    std::optional<Clock::time_point> m_zero_since;
    std::optional<Clock::time_point> m_last_recovery_retry;
};

/** One instance per GUI node. All access must run on the owning GUI thread. */
class BootstrapManager
{
public:
    using Clock = BootstrapPolicy::Clock;
    using Connector = std::function<bool(const std::string&, bool)>;
    explicit BootstrapManager(interfaces::Node& node, Connector connector = {}, bool persist = true);
    ~BootstrapManager();
    void poll(Clock::time_point now = Clock::now());
    static std::optional<std::string> normalizeAddress(const std::string& input);
    bool saveAddress(const std::string& input);
    bool removeAddress(const std::string& address);
    bool connectAddress(const std::string& address);
    const std::vector<std::string>& recoveryAddresses() const { return m_recovery; }
    CConnman::OneTryStatus recoveryStatus(const std::string& address) const;

private:
    bool saveSettings();
    const bool m_persist;
    std::vector<std::string> m_recovery;
    void release();
    interfaces::Node& m_node;
    Connector m_connector;
    BootstrapPolicy m_policy;
    std::set<CNetAddr> m_addresses;
    std::set<int64_t> m_bootstrap_ids;
    std::set<std::string> m_owned_seeds;
    bool m_started{false};
    Clock::time_point m_next_poll{};
};

#endif // BITCOIN_QT_BOOTSTRAPMANAGER_H
