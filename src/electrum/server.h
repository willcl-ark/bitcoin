// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_ELECTRUM_SERVER_H
#define BITCOIN_ELECTRUM_SERVER_H

#include <sync.h>
#include <uint256.h>
#include <util/hasher.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>

struct event;
struct event_base;
struct evconnlistener;
struct bufferevent;

class MempoolScriptHashIndex;
class UniValue;

namespace node {
struct NodeContext;
}

static constexpr uint16_t DEFAULT_ELECTRUM_PORT{50001};
static constexpr const char* DEFAULT_ELECTRUM_BIND{"127.0.0.1"};
static constexpr size_t DEFAULT_ELECTRUM_MAX_CONNECTIONS{5};
static constexpr size_t MAX_ELECTRUM_REQUEST_SIZE{1024 * 1024};
static constexpr double ELECTRUM_REQUESTS_PER_SECOND{20.0};
static constexpr double ELECTRUM_REQUEST_BURST{40.0};

class ElectrumServer
{
public:
    using MethodHandler = std::function<UniValue(struct bufferevent* bev, const UniValue& params)>;

    ElectrumServer(node::NodeContext& node, std::string bind_addr, uint16_t port, size_t max_connections);
    ~ElectrumServer();

    bool Start();
    void Interrupt();
    void Stop();

    void RegisterMethod(const std::string& name, MethodHandler handler);
    node::NodeContext& GetNode() { return m_node; }
    MempoolScriptHashIndex* GetMempoolIndex() { return m_mempool_index.get(); }

    void SubscribeHeaders(struct bufferevent* bev);
    void SubscribeScripthash(struct bufferevent* bev, const uint256& scripthash,
                             const std::optional<std::string>& initial_status);
    bool UnsubscribeScripthash(struct bufferevent* bev, const uint256& scripthash);

private:
    node::NodeContext& m_node;
    std::shared_ptr<MempoolScriptHashIndex> m_mempool_index;
    std::string m_bind_addr;
    uint16_t m_port;
    size_t m_max_connections;

    struct event_base* m_event_base{nullptr};
    struct evconnlistener* m_listener{nullptr};
    std::thread m_thread;

    GlobalMutex m_connections_mutex;
    std::set<struct bufferevent*> m_connections GUARDED_BY(m_connections_mutex);
    struct ConnectionState {
        double tokens{ELECTRUM_REQUEST_BURST};
        std::chrono::steady_clock::time_point last_refill{std::chrono::steady_clock::now()};
        bool subscribed_headers{false};
        std::unordered_map<uint256, std::optional<std::string>, SaltedUint256Hasher> scripthash_subs;
    };
    std::unordered_map<struct bufferevent*, ConnectionState> m_connection_state GUARDED_BY(m_connections_mutex);

    struct event* m_notify_timer{nullptr};
    uint256 m_last_tip_hash;

    std::unordered_map<std::string, MethodHandler> m_methods;

    void RegisterMethods();
    void EventLoop();
    bool ConsumeRequestToken(struct bufferevent* bev) EXCLUSIVE_LOCKS_REQUIRED(m_connections_mutex);
    void HandleLine(struct bufferevent* bev, const std::string& line);
    void SendResponse(struct bufferevent* bev, const UniValue& response);

    void StartMempoolIndex();
    void StopMempoolIndex();
    void ProcessNotifications();

    static void AcceptCb(struct evconnlistener* listener, int fd,
                         struct sockaddr* addr, int socklen, void* ctx);
    static void ReadCb(struct bufferevent* bev, void* ctx);
    static void EventCb(struct bufferevent* bev, short events, void* ctx);
    static void NotifyTimerCb(int fd, short events, void* ctx);
};

#endif // BITCOIN_ELECTRUM_SERVER_H
