// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <electrum/server.h>

#include <logging.h>
#include <netbase.h>
#include <sync.h>
#include <univalue.h>
#include <util/threadnames.h>

#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/event.h>
#include <event2/listener.h>
#include <event2/thread.h>
#include <event2/util.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <string>

class ElectrumServer;
void RegisterElectrumMethods(ElectrumServer& server);

void ElectrumServer::UncacheConnectionScripthashes(const ConnectionState& state)
{
    if (!m_on_scripthash_unsubscribe) return;
    for (const auto& [scripthash, _] : state.scripthash_subs) {
        m_on_scripthash_unsubscribe(scripthash);
    }
}

ElectrumServer::ElectrumServer(node::NodeContext& node, std::string bind_addr, uint16_t port, size_t max_connections)
    : m_node(node), m_bind_addr(std::move(bind_addr)), m_port(port), m_max_connections(max_connections)
{
}

ElectrumServer::~ElectrumServer()
{
    Stop();
}

void ElectrumServer::RegisterMethod(const std::string& name, MethodHandler handler)
{
    m_methods[name] = std::move(handler);
}

void ElectrumServer::SetScripthashSubscriptionCallbacks(std::function<void(const uint256&)> on_subscribe,
                                                        std::function<void(const uint256&)> on_unsubscribe)
{
    m_on_scripthash_subscribe = std::move(on_subscribe);
    m_on_scripthash_unsubscribe = std::move(on_unsubscribe);
}

void ElectrumServer::RegisterMethods()
{
    RegisterElectrumMethods(*this);
}

bool ElectrumServer::Start()
{
#ifdef WIN32
    evthread_use_windows_threads();
#else
    evthread_use_pthreads();
#endif

    m_event_base = event_base_new();
    if (!m_event_base) {
        LogError("Electrum: failed to create event base");
        return false;
    }

    const std::string bind_addr = m_bind_addr.empty() ? DEFAULT_ELECTRUM_BIND : m_bind_addr;
    const std::optional<CService> service = Lookup(bind_addr, m_port, /*fAllowLookup=*/false);
    if (!service) {
        LogError("Electrum: invalid bind address '%s'", m_bind_addr);
        event_base_free(m_event_base);
        m_event_base = nullptr;
        return false;
    }

    struct sockaddr_storage ss{};
    socklen_t ss_len = sizeof(ss);
    if (!service->GetSockAddr(reinterpret_cast<struct sockaddr*>(&ss), &ss_len)) {
        LogError("Electrum: unsupported address type '%s'", m_bind_addr);
        event_base_free(m_event_base);
        m_event_base = nullptr;
        return false;
    }

    m_listener = evconnlistener_new_bind(
        m_event_base, AcceptCb, this,
        LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE, -1,
        reinterpret_cast<struct sockaddr*>(&ss), ss_len);

    if (!m_listener) {
        LogError("Electrum: failed to bind on %s:%d", m_bind_addr, m_port);
        event_base_free(m_event_base);
        m_event_base = nullptr;
        return false;
    }

    RegisterMethods();
    StartMempoolIndex();

    m_notify_timer = event_new(m_event_base, -1, EV_PERSIST, NotifyTimerCb, this);
    struct timeval one_sec{1, 0};
    event_add(m_notify_timer, &one_sec);

    m_thread = std::thread(&ElectrumServer::EventLoop, this);
    LogInfo("Electrum server listening on %s:%d", m_bind_addr, m_port);
    return true;
}

void ElectrumServer::Interrupt()
{
    if (m_event_base) {
        event_base_loopbreak(m_event_base);
    }
}

void ElectrumServer::Stop()
{
    if (m_thread.joinable()) {
        Interrupt();
        m_thread.join();
    }

    StopMempoolIndex();

    if (m_notify_timer) {
        event_free(m_notify_timer);
        m_notify_timer = nullptr;
    }

    {
        LOCK(m_connections_mutex);
        for (const auto& [bev, state] : m_connection_state) {
            UncacheConnectionScripthashes(state);
            (void)bev;
        }
        for (auto* bev : m_connections) {
            bufferevent_free(bev);
        }
        m_connections.clear();
        m_connection_state.clear();
    }

    if (m_listener) {
        evconnlistener_free(m_listener);
        m_listener = nullptr;
    }

    if (m_event_base) {
        event_base_free(m_event_base);
        m_event_base = nullptr;
    }
}

void ElectrumServer::EventLoop()
{
    util::ThreadRename("electrum");
    LogInfo("Electrum event loop started");
    event_base_dispatch(m_event_base);
    LogInfo("Electrum event loop exited");
}

void ElectrumServer::AcceptCb(struct evconnlistener* /*listener*/, int fd,
                               struct sockaddr* /*addr*/, int /*socklen*/, void* ctx)
{
    auto* server = static_cast<ElectrumServer*>(ctx);

    struct bufferevent* bev = bufferevent_socket_new(
        server->m_event_base, fd, BEV_OPT_CLOSE_ON_FREE);

    if (!bev) {
        LogWarning("Electrum: failed to create bufferevent for new connection");
        evutil_closesocket(fd);
        return;
    }

    bufferevent_setcb(bev, ReadCb, nullptr, EventCb, ctx);
    bufferevent_enable(bev, EV_READ | EV_WRITE);

    {
        LOCK(server->m_connections_mutex);
        if (server->m_connections.size() >= server->m_max_connections) {
            LogWarning("Electrum: max connections (%zu) reached, rejecting new connection", server->m_max_connections);
            bufferevent_free(bev);
            return;
        }
        server->m_connections.insert(bev);
        server->m_connection_state.emplace(bev, ConnectionState{});
    }
}

void ElectrumServer::ReadCb(struct bufferevent* bev, void* ctx)
{
    auto* server = static_cast<ElectrumServer*>(ctx);
    struct evbuffer* input = bufferevent_get_input(bev);

    for (;;) {
        size_t len = evbuffer_get_length(input);
        if (len == 0) break;

        if (len > MAX_ELECTRUM_REQUEST_SIZE) {
            LogWarning("Electrum: request too large, disconnecting client");
            LOCK(server->m_connections_mutex);
            auto it = server->m_connection_state.find(bev);
            if (it != server->m_connection_state.end()) {
                server->UncacheConnectionScripthashes(it->second);
                server->m_connection_state.erase(it);
            }
            server->m_connections.erase(bev);
            bufferevent_free(bev);
            return;
        }

        char* line = evbuffer_readln(input, nullptr, EVBUFFER_EOL_LF);
        if (!line) break;

        std::string request(line);
        free(line);

        {
            LOCK(server->m_connections_mutex);
            if (!server->ConsumeRequestToken(bev)) {
                LogWarning("Electrum: request rate limit exceeded, disconnecting client");
                auto it = server->m_connection_state.find(bev);
                if (it != server->m_connection_state.end()) {
                    server->UncacheConnectionScripthashes(it->second);
                    server->m_connection_state.erase(it);
                }
                server->m_connections.erase(bev);
                bufferevent_free(bev);
                return;
            }
        }

        server->HandleLine(bev, request);
    }
}

void ElectrumServer::EventCb(struct bufferevent* bev, short events, void* ctx)
{
    auto* server = static_cast<ElectrumServer*>(ctx);

    if (events & (BEV_EVENT_EOF | BEV_EVENT_ERROR)) {
        LOCK(server->m_connections_mutex);
        auto it = server->m_connection_state.find(bev);
        if (it != server->m_connection_state.end()) {
            server->UncacheConnectionScripthashes(it->second);
            server->m_connection_state.erase(it);
        }
        server->m_connections.erase(bev);
        bufferevent_free(bev);
    }
}

bool ElectrumServer::ConsumeRequestToken(struct bufferevent* bev)
{
    const auto it = m_connection_state.find(bev);
    if (it == m_connection_state.end()) return false;

    const auto now = std::chrono::steady_clock::now();
    const std::chrono::duration<double> elapsed = now - it->second.last_refill;
    it->second.tokens = std::min(ELECTRUM_REQUEST_BURST, it->second.tokens + elapsed.count() * ELECTRUM_REQUESTS_PER_SECOND);
    it->second.last_refill = now;

    if (it->second.tokens < 1.0) return false;

    it->second.tokens -= 1.0;
    return true;
}

void ElectrumServer::HandleLine(struct bufferevent* bev, const std::string& line)
{
    UniValue request;
    if (!request.read(line)) {
        UniValue error(UniValue::VOBJ);
        error.pushKV("jsonrpc", "2.0");
        UniValue err_obj(UniValue::VOBJ);
        err_obj.pushKV("code", -32700);
        err_obj.pushKV("message", "Parse error");
        error.pushKV("error", err_obj);
        error.pushKV("id", UniValue());
        SendResponse(bev, error);
        return;
    }

    UniValue response(UniValue::VOBJ);
    response.pushKV("jsonrpc", "2.0");

    UniValue id;
    try {
        id = request["id"];
        const std::string method = request["method"].get_str();
        const UniValue& params = request["params"];

        auto it = m_methods.find(method);
        if (it == m_methods.end()) {
            UniValue err_obj(UniValue::VOBJ);
            err_obj.pushKV("code", -32601);
            err_obj.pushKV("message", "Method not found");
            response.pushKV("error", err_obj);
        } else {
            UniValue result = it->second(bev, params);
            response.pushKV("result", result);
        }
    } catch (const std::exception& e) {
        UniValue err_obj(UniValue::VOBJ);
        err_obj.pushKV("code", -32000);
        err_obj.pushKV("message", std::string(e.what()));
        response.pushKV("error", err_obj);
    }

    response.pushKV("id", id);
    SendResponse(bev, response);
}

void ElectrumServer::SendResponse(struct bufferevent* bev, const UniValue& response)
{
    std::string data = response.write() + "\n";
    bufferevent_write(bev, data.data(), data.size());
}

void ElectrumServer::SubscribeHeaders(struct bufferevent* bev)
{
    LOCK(m_connections_mutex);
    auto it = m_connection_state.find(bev);
    if (it != m_connection_state.end()) {
        it->second.subscribed_headers = true;
    }
}

void ElectrumServer::SubscribeScripthash(struct bufferevent* bev, const uint256& scripthash,
                                          const std::optional<std::string>& initial_status)
{
    SubscribeScripthashes(bev, {{scripthash, initial_status}});
}

void ElectrumServer::SubscribeScripthashes(struct bufferevent* bev, const std::vector<std::pair<uint256, std::optional<std::string>>>& subscriptions)
{
    std::vector<uint256> inserted;
    {
        LOCK(m_connections_mutex);
        auto it = m_connection_state.find(bev);
        if (it == m_connection_state.end()) return;

        inserted.reserve(subscriptions.size());
        for (const auto& [scripthash, initial_status] : subscriptions) {
            if (!it->second.scripthash_subs.contains(scripthash)) {
                inserted.push_back(scripthash);
            }
            it->second.scripthash_subs[scripthash] = initial_status;
        }
    }

    if (!m_on_scripthash_subscribe) return;
    for (const auto& scripthash : inserted) {
        m_on_scripthash_subscribe(scripthash);
    }
}

bool ElectrumServer::UnsubscribeScripthash(struct bufferevent* bev, const uint256& scripthash)
{
    LOCK(m_connections_mutex);
    auto it = m_connection_state.find(bev);
    if (it == m_connection_state.end()) return false;
    const bool erased{it->second.scripthash_subs.erase(scripthash) > 0};
    if (erased && m_on_scripthash_unsubscribe) {
        m_on_scripthash_unsubscribe(scripthash);
    }
    return erased;
}
