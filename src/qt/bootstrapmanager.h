// Copyright (c) 2026 The ReSatoshi developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_QT_BOOTSTRAPMANAGER_H
#define BITCOIN_QT_BOOTSTRAPMANAGER_H

#include <netaddress.h>

#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace interfaces { class Node; }

/** Monotonic-time policy; each replacement peer must itself stay ready for 60s. */
class BootstrapPolicy
{
public:
    using Clock = std::chrono::steady_clock;
    struct Peer { int64_t id; bool bootstrap; bool ready; };
    struct Action { std::vector<int64_t> disconnect; bool reconnect{false}; bool release{false}; };
    Action update(Clock::time_point now, const std::vector<Peer>& peers, bool network_active, bool synchronized = true);

private:
    std::map<int64_t, Clock::time_point> m_ready_since;
    std::optional<Clock::time_point> m_last_retry;
    bool m_had_peers{false};
};

/** One instance per GUI node, shared by all its wallet dashboards. */
class BootstrapManager
{
public:
    using Clock = BootstrapPolicy::Clock;
    using Connector = std::function<bool(const std::string&, bool)>;
    explicit BootstrapManager(interfaces::Node& node, Connector connector = {});
    ~BootstrapManager();
    void poll(Clock::time_point now = Clock::now());

private:
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
