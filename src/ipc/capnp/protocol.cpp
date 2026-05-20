// Copyright (c) 2021-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <interfaces/init.h>
#include <ipc/capnp/echo.capnp.h>
#include <ipc/capnp/context.h>
#include <ipc/capnp/init.capnp.h>
#include <ipc/capnp/init.capnp.proxy.h>
#include <ipc/capnp/protocol.h>
#include <ipc/exception.h>
#include <ipc/protocol.h>

#include <capnp/rpc-twoparty.h>

#include <kj/async-io.h>
#include <kj/async.h>
#include <kj/debug.h>
#include <logging.h>
#include <mp/proxy-io.h>
#include <mp/proxy-types.h>
#include <mp/util.h>
#include <util/threadnames.h>

#include <cassert>
#include <cerrno>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <sys/socket.h>
#include <system_error>
#include <thread>
#include <utility>

namespace ipc {
namespace capnp {
namespace {

mp::Log GetRequestedIPCLogLevel()
{
    if (LogAcceptCategory(BCLog::IPC, BCLog::Level::Trace)) return mp::Log::Trace;
    if (LogAcceptCategory(BCLog::IPC, BCLog::Level::Debug)) return mp::Log::Debug;

    // Info, Warning, and Error are logged unconditionally
    return mp::Log::Info;
}

void IpcLogFn(mp::LogMessage message)
{
    switch (message.level) {
    case mp::Log::Trace:
        LogTrace(BCLog::IPC, "%s", message.message);
        return;
    case mp::Log::Debug:
        LogDebug(BCLog::IPC, "%s", message.message);
        return;
    case mp::Log::Info:
        LogInfo("ipc: %s", message.message);
        return;
    case mp::Log::Warning:
        LogWarning("ipc: %s", message.message);
        return;
    case mp::Log::Error:
        LogError("ipc: %s", message.message);
        return;
    case mp::Log::Raise:
        LogError("ipc: %s", message.message);
        throw Exception(message.message);
    } // no default case, so the compiler can warn about missing cases

    // Be conservative and assume that if MP ever adds a new log level, it
    // should only be shown at our most verbose level.
    LogTrace(BCLog::IPC, "%s", message.message);
}

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
    kj::Promise<void> destroy(DestroyContext) override
    {
        m_echo.reset();
        return kj::READY_NOW;
    }

    kj::Promise<void> echo(EchoContext context) override
    {
        KJ_REQUIRE(m_echo != nullptr, "Echo interface was already destroyed.");
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

class CapnpProtocol : public Protocol
{
public:
    ~CapnpProtocol() noexcept(true)
    {
        m_loop_ref.reset();
        if (m_loop_thread.joinable()) m_loop_thread.join();
        assert(!m_loop);
    };
    std::unique_ptr<interfaces::Init> connect(int fd, const char* exe_name) override
    {
        startLoop(exe_name);
        return mp::ConnectStream<messages::Init>(*m_loop, fd);
    }
    void listen(int listen_fd, const char* exe_name, interfaces::Init& init) override
    {
        startLoop(exe_name);
        if (::listen(listen_fd, /*backlog=*/5) != 0) {
            throw std::system_error(errno, std::system_category());
        }
        mp::ListenConnections<messages::Init>(*m_loop, listen_fd, init);
    }
    void serve(int fd, const char* exe_name, interfaces::Init& init, const std::function<void()>& ready_fn = {}) override
    {
        assert(!m_loop);
        mp::g_thread_context.thread_name = mp::ThreadName(exe_name);
        mp::LogOptions opts = {
            .log_fn = IpcLogFn,
            .log_level = GetRequestedIPCLogLevel()
        };
        m_loop.emplace(exe_name, std::move(opts), &m_context);
        if (ready_fn) ready_fn();
        mp::ServeStream<messages::Init>(*m_loop, fd, init);
        m_parent_connection = &m_loop->m_incoming_connections.back();
        m_loop->loop();
        m_loop.reset();
    }
    void disconnectIncoming() override
    {
        if (!m_loop) return;
        // Delete incoming connections, except the connection to a parent
        // process (if there is one), since a parent process should be able to
        // monitor and control this process, even during shutdown.
        m_loop->sync([&] {
            m_loop->m_incoming_connections.remove_if([this](mp::Connection& c) { return &c != m_parent_connection; });
        });
    }
    void addCleanup(std::type_index type, void* iface, std::function<void()> cleanup) override
    {
        mp::ProxyTypeRegister::types().at(type)(iface).cleanup_fns.emplace_back(std::move(cleanup));
    }
    Context& context() override { return m_context; }
    void startLoop(const char* exe_name)
    {
        if (m_loop) return;
        std::promise<void> promise;
        m_loop_thread = std::thread([&] {
            util::ThreadRename("capnp-loop");
            mp::LogOptions opts = {
                .log_fn = IpcLogFn,
                .log_level = GetRequestedIPCLogLevel()
            };
            m_loop.emplace(exe_name, std::move(opts), &m_context);
            m_loop_ref.emplace(*m_loop);
            promise.set_value();
            m_loop->loop();
            m_loop.reset();
        });
        promise.get_future().wait();
    }
    Context m_context;
    std::thread m_loop_thread;
    //! EventLoop object which manages I/O events for all connections.
    std::optional<mp::EventLoop> m_loop;
    //! Reference to the same EventLoop. Increments the loop’s refcount on
    //! creation, decrements on destruction. The loop thread exits when the
    //! refcount reaches 0. Other IPC objects also hold their own EventLoopRef.
    std::optional<mp::EventLoopRef> m_loop_ref;
    //! Connection to parent, if this is a child process spawned by a parent process.
    mp::Connection* m_parent_connection{nullptr};
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
