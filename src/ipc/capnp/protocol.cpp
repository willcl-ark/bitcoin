// Copyright (c) 2021-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <interfaces/echo.h>
#include <interfaces/init.h>
#include <ipc/capnp/echo.capnp.h>
#include <ipc/capnp/context.h>
#include <ipc/capnp/init.capnp.h>
#include <ipc/capnp/protocol.h>
#include <ipc/protocol.h>

#include <capnp/rpc-twoparty.h>

#include <kj/async-io.h>
#include <kj/async.h>
#include <kj/debug.h>
#include <logging.h>
#include <util/threadnames.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <memory>
#include <mutex>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <system_error>
#include <thread>
#include <typeinfo>
#include <unistd.h>
#include <utility>
#include <vector>

namespace ipc {
namespace capnp {
namespace {

struct NativeServerVatId
{
    ::capnp::word scratch[4]{};
    ::capnp::MallocMessageBuilder message{scratch};
    ::capnp::rpc::twoparty::VatId::Builder vat_id{message.getRoot<::capnp::rpc::twoparty::VatId>()};
    NativeServerVatId() { vat_id.setSide(::capnp::rpc::twoparty::Side::SERVER); }
};

class EchoServer final : public messages::Echo::Server
{
public:
    explicit EchoServer(std::unique_ptr<interfaces::Echo> echo) : m_echo{std::move(echo)} {}

protected:
    kj::Promise<void> echo(EchoContext context) override
    {
        const std::string result{m_echo->echo(context.getParams().getEcho().cStr())};
        context.getResults().setResult(result);
        return kj::READY_NOW;
    }

private:
    std::unique_ptr<interfaces::Echo> m_echo;
};

class InitServer final : public messages::Init::Server
{
public:
    explicit InitServer(interfaces::Init& init) : m_init{init} {}

protected:
    kj::Promise<void> makeEcho(MakeEchoContext context) override
    {
        auto echo{m_init.makeEcho()};
        KJ_REQUIRE(echo != nullptr, "Init::makeEcho returned null.");
        context.getResults().setResult(messages::Echo::Client{kj::heap<EchoServer>(std::move(echo))});
        return kj::READY_NOW;
    }

private:
    interfaces::Init& m_init;
};

struct ClientContext {
    explicit ClientContext(int fd) : connection{ConnectNative(fd)} {}

    std::unique_ptr<NativeConnection> connection;
    std::mutex mutex;
};

class CleanupHandler
{
public:
    void addCleanup(std::function<void()> cleanup) { m_cleanups.emplace_back(std::move(cleanup)); }

protected:
    virtual ~CleanupHandler() noexcept
    {
        for (auto& cleanup : m_cleanups) {
            try {
                cleanup();
            } catch (const std::exception& e) {
                LogError("ipc: cleanup failed: %s", e.what());
            }
        }
    }

private:
    std::vector<std::function<void()>> m_cleanups;
};

class EchoClient final : public interfaces::Echo, public CleanupHandler
{
public:
    EchoClient(messages::Echo::Client echo, std::shared_ptr<ClientContext> context)
        : m_echo{kj::mv(echo)}, m_context{std::move(context)}
    {
    }
    ~EchoClient() noexcept override = default;

    std::string echo(const std::string& message) override
    {
        std::lock_guard<std::mutex> lock{m_context->mutex};
        auto request{m_echo.echoRequest()};
        request.setEcho(message);
        auto response{request.send().wait(m_context->connection->waitScope())};
        return response.getResult().cStr();
    }

private:
    messages::Echo::Client m_echo;
    std::shared_ptr<ClientContext> m_context;
};

class InitClient final : public interfaces::Init, public CleanupHandler
{
public:
    explicit InitClient(int fd) : m_context{std::make_shared<ClientContext>(fd)} {}
    ~InitClient() noexcept override = default;

    std::unique_ptr<interfaces::Echo> makeEcho() override
    {
        std::lock_guard<std::mutex> lock{m_context->mutex};
        auto response{m_context->connection->init().makeEchoRequest().send().wait(m_context->connection->waitScope())};
        return std::make_unique<EchoClient>(response.getResult(), m_context);
    }

private:
    std::shared_ptr<ClientContext> m_context;
};

class ServerConnection
{
public:
    explicit ServerConnection(int fd) : m_fd{fd} {}
    ~ServerConnection() { close(); }

    void shutdown()
    {
        std::lock_guard<std::mutex> lock{m_mutex};
        if (m_fd >= 0) (void)::shutdown(m_fd, SHUT_RDWR);
    }

    void close()
    {
        std::lock_guard<std::mutex> lock{m_mutex};
        if (m_fd >= 0) {
            (void)::close(m_fd);
            m_fd = -1;
        }
    }

private:
    std::mutex m_mutex;
    int m_fd{-1};
};

class CapnpProtocol : public Protocol
{
public:
    ~CapnpProtocol() noexcept(true) override
    {
        disconnectIncoming();
        joinThreads();
    }

    std::unique_ptr<interfaces::Init> connect(int fd, const char*) override
    {
        return std::make_unique<InitClient>(fd);
    }

    void listen(int listen_fd, const char*, interfaces::Init& init) override
    {
        if (::listen(listen_fd, /*backlog=*/5) != 0) {
            throw std::system_error(errno, std::system_category());
        }
        std::lock_guard<std::mutex> lock{m_mutex};
        m_listeners.push_back({listen_fd, {}});
        m_listeners.back().thread = std::thread([this, listen_fd, &init] {
            util::ThreadRename("ipc-listen");
            acceptLoop(listen_fd, init);
        });
    }

    void serve(int fd, const char*, interfaces::Init& init, const std::function<void()>& ready_fn = {}) override
    {
        ServeNative(fd, init, ready_fn);
    }

    void disconnectIncoming() override
    {
        m_stop = true;
        closeListeners();
        shutdownConnections();
    }

    void addCleanup(std::type_index type, void* iface, std::function<void()> cleanup) override
    {
        CleanupHandler* handler{nullptr};
        if (type == typeid(interfaces::Init)) {
            handler = dynamic_cast<CleanupHandler*>(static_cast<interfaces::Init*>(iface));
        } else if (type == typeid(interfaces::Echo)) {
            handler = dynamic_cast<CleanupHandler*>(static_cast<interfaces::Echo*>(iface));
        }
        if (!handler) throw std::runtime_error("IPC cleanup can only be added to native IPC clients.");
        handler->addCleanup(std::move(cleanup));
    }

    Context& context() override { return m_context; }

private:
    struct Listener {
        int fd{-1};
        std::thread thread;
    };

    void acceptLoop(int listen_fd, interfaces::Init& init)
    {
        while (!m_stop) {
            pollfd poll_fd{listen_fd, POLLIN, 0};
            const int poll_result{::poll(&poll_fd, 1, 100)};
            if (poll_result < 0) {
                if (errno == EINTR) continue;
                if (!m_stop) LogError("ipc: listen poll failed: %s", std::strerror(errno));
                return;
            }
            if (poll_result == 0 || !(poll_fd.revents & POLLIN)) continue;

            const int connection_fd{::accept(listen_fd, nullptr, nullptr)};
            if (connection_fd < 0) {
                if (errno == EINTR) continue;
                if (!m_stop) LogError("ipc: accept failed: %s", std::strerror(errno));
                continue;
            }

            const int shutdown_fd{::dup(connection_fd)};
            if (shutdown_fd < 0) {
                LogError("ipc: dup failed: %s", std::strerror(errno));
                (void)::close(connection_fd);
                continue;
            }

            auto connection{std::make_shared<ServerConnection>(shutdown_fd)};
            std::lock_guard<std::mutex> lock{m_mutex};
            if (m_stop) {
                connection->close();
                (void)::close(connection_fd);
                return;
            }
            m_connections.push_back(connection);
            m_server_threads.emplace_back([connection_fd, connection, &init] {
                util::ThreadRename("ipc-serve");
                try {
                    ServeNative(connection_fd, init);
                } catch (const std::exception& e) {
                    LogError("ipc: server connection failed: %s", e.what());
                }
                connection->close();
            });
        }
    }

    void closeListeners() noexcept
    {
        std::lock_guard<std::mutex> lock{m_mutex};
        for (auto& listener : m_listeners) {
            if (listener.fd >= 0) {
                (void)::close(listener.fd);
                listener.fd = -1;
            }
        }
    }

    void shutdownConnections() noexcept
    {
        std::lock_guard<std::mutex> lock{m_mutex};
        for (auto& connection : m_connections) {
            connection->shutdown();
        }
    }

    void joinThreads() noexcept
    {
        for (auto& listener : m_listeners) {
            if (listener.thread.joinable()) listener.thread.join();
        }
        shutdownConnections();
        for (auto& thread : m_server_threads) {
            if (thread.joinable()) thread.join();
        }
    }

    Context m_context;
    std::atomic<bool> m_stop{false};
    std::mutex m_mutex;
    std::vector<Listener> m_listeners;
    std::vector<std::thread> m_server_threads;
    std::vector<std::shared_ptr<ServerConnection>> m_connections;
};
} // namespace

class NativeConnection::Impl
{
public:
    explicit Impl(int fd)
        : m_io_context{kj::setupAsyncIo()},
          m_stream{m_io_context.lowLevelProvider->wrapSocketFd(fd, kj::LowLevelAsyncIoProvider::TAKE_OWNERSHIP)},
          m_network{*m_stream, ::capnp::rpc::twoparty::Side::CLIENT, ::capnp::ReaderOptions{}},
          m_rpc_system{::capnp::makeRpcClient(m_network)},
          m_init{m_rpc_system.bootstrap(NativeServerVatId().vat_id).castAs<messages::Init>()}
    {
    }

    messages::Init::Client init() { return m_init; }
    kj::WaitScope& waitScope() { return m_io_context.waitScope; }

private:
    kj::AsyncIoContext m_io_context;
    kj::Own<kj::AsyncIoStream> m_stream;
    ::capnp::TwoPartyVatNetwork m_network;
    ::capnp::RpcSystem<::capnp::rpc::twoparty::VatId> m_rpc_system;
    messages::Init::Client m_init;
};

NativeConnection::NativeConnection(int fd) : m_impl{std::make_unique<Impl>(fd)} {}
NativeConnection::~NativeConnection() = default;

messages::Init::Client NativeConnection::init()
{
    return m_impl->init();
}

kj::WaitScope& NativeConnection::waitScope()
{
    return m_impl->waitScope();
}

std::unique_ptr<NativeConnection> ConnectNative(int fd)
{
    return std::make_unique<NativeConnection>(fd);
}

void ServeNative(int fd, interfaces::Init& init, const std::function<void()>& ready_fn)
{
    auto io_context{kj::setupAsyncIo()};
    auto stream{io_context.lowLevelProvider->wrapSocketFd(fd, kj::LowLevelAsyncIoProvider::TAKE_OWNERSHIP)};
    ::capnp::TwoPartyVatNetwork network{*stream, ::capnp::rpc::twoparty::Side::SERVER, ::capnp::ReaderOptions{}};
    ::capnp::Capability::Client bootstrap{kj::heap<InitServer>(init)};
    auto rpc_system{::capnp::makeRpcServer(network, kj::mv(bootstrap))};
    (void)rpc_system;
    if (ready_fn) ready_fn();
    network.onDisconnect().wait(io_context.waitScope);
}

std::unique_ptr<Protocol> MakeCapnpProtocol() { return std::make_unique<CapnpProtocol>(); }
} // namespace capnp
} // namespace ipc
