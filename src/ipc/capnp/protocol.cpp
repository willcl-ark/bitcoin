// Copyright (c) 2021-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <interfaces/echo.h>
#include <interfaces/init.h>
#include <interfaces/mining.h>
#include <interfaces/rpc.h>
#include <ipc/capnp/conversions.h>
#include <ipc/capnp/echo.capnp.h>
#include <ipc/capnp/context.h>
#include <ipc/capnp/init.capnp.h>
#include <ipc/capnp/protocol.h>
#include <ipc/capnp/rpc.capnp.h>
#include <ipc/capnp/worker_queue.h>
#include <ipc/protocol.h>

#include <capnp/rpc-twoparty.h>

#include <kj/async-io.h>
#include <kj/async.h>
#include <kj/debug.h>
#include <logging.h>
#include <util/threadnames.h>

#include <atomic>
#include <cassert>
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
#include <tuple>
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

class RpcServer final : public messages::Rpc::Server
{
public:
    RpcServer(std::unique_ptr<interfaces::Rpc> rpc, std::shared_ptr<WorkerQueue> worker_queue)
        : m_rpc{std::move(rpc)}, m_worker_queue{std::move(worker_queue)}
    {
    }

protected:
    kj::Promise<void> executeRpc(ExecuteRpcContext context) override
    {
        auto params{context.getParams()};
        std::string request{params.getRequest().cStr()};
        std::string uri{params.getUri().cStr()};
        std::string user{params.getUser().cStr()};
        return m_worker_queue->post([rpc = m_rpc, request = std::move(request), uri = std::move(uri), user = std::move(user)] {
            return WriteUniValue(rpc->executeRpc(ReadUniValue(request), uri, user));
        }).then([context = kj::mv(context)](std::string result) mutable {
            context.getResults().setResult(result);
        });
    }

private:
    std::shared_ptr<interfaces::Rpc> m_rpc;
    std::shared_ptr<WorkerQueue> m_worker_queue;
};

class BlockTemplateServer final : public messages::BlockTemplate::Server
{
public:
    BlockTemplateServer(std::unique_ptr<interfaces::BlockTemplate> block_template, std::shared_ptr<WorkerQueue> worker_queue)
        : m_block_template{std::move(block_template)}, m_worker_queue{std::move(worker_queue)}
    {
    }

protected:
    kj::Promise<void> getBlockHeader(GetBlockHeaderContext context) override
    {
        return m_worker_queue->post([block_template = m_block_template] {
            return SerializeData(block_template->getBlockHeader());
        }).then([context = kj::mv(context)](std::vector<unsigned char> result) mutable {
            context.getResults().setResult(MakeDataReader(result));
        });
    }

    kj::Promise<void> getBlock(GetBlockContext context) override
    {
        return m_worker_queue->post([block_template = m_block_template] {
            return SerializeData(block_template->getBlock());
        }).then([context = kj::mv(context)](std::vector<unsigned char> result) mutable {
            context.getResults().setResult(MakeDataReader(result));
        });
    }

    kj::Promise<void> getTxFees(GetTxFeesContext context) override
    {
        return m_worker_queue->post([block_template = m_block_template] {
            return block_template->getTxFees();
        }).then([context = kj::mv(context)](std::vector<CAmount> result) mutable {
            auto output{context.getResults().initResult(result.size())};
            for (size_t i{0}; i < result.size(); ++i) {
                output.set(i, result[i]);
            }
        });
    }

    kj::Promise<void> getTxSigops(GetTxSigopsContext context) override
    {
        return m_worker_queue->post([block_template = m_block_template] {
            return block_template->getTxSigops();
        }).then([context = kj::mv(context)](std::vector<int64_t> result) mutable {
            auto output{context.getResults().initResult(result.size())};
            for (size_t i{0}; i < result.size(); ++i) {
                output.set(i, result[i]);
            }
        });
    }

    kj::Promise<void> getCoinbaseTx(GetCoinbaseTxContext context) override
    {
        return m_worker_queue->post([block_template = m_block_template] {
            return block_template->getCoinbaseTx();
        }).then([context = kj::mv(context)](node::CoinbaseTx result) mutable {
            BuildCoinbaseTx(context.getResults().initResult(), result);
        });
    }

    kj::Promise<void> getCoinbaseMerklePath(GetCoinbaseMerklePathContext context) override
    {
        return m_worker_queue->post([block_template = m_block_template] {
            return block_template->getCoinbaseMerklePath();
        }).then([context = kj::mv(context)](std::vector<uint256> result) mutable {
            std::vector<::capnp::Data::Reader> readers;
            readers.reserve(result.size());
            for (const auto& hash : result) {
                readers.push_back(MakeDataReader({hash.data(), hash.size()}));
            }
            context.getResults().setResult({readers.data(), readers.size()});
        });
    }

    kj::Promise<void> submitSolution(SubmitSolutionContext context) override
    {
        auto params{context.getParams()};
        const uint32_t version{params.getVersion()};
        const uint32_t timestamp{params.getTimestamp()};
        const uint32_t nonce{params.getNonce()};
        CTransactionRef coinbase{ReadTransaction(params.getCoinbase())};
        return m_worker_queue->post([block_template = m_block_template, version, timestamp, nonce, coinbase = std::move(coinbase)] {
            return block_template->submitSolution(version, timestamp, nonce, coinbase);
        }).then([context = kj::mv(context)](bool result) mutable {
            context.getResults().setResult(result);
        });
    }

    kj::Promise<void> waitNext(WaitNextContext context) override
    {
        node::BlockWaitOptions options{ReadBlockWaitOptions(context.getParams().getOptions())};
        return m_worker_queue->post([block_template = m_block_template, options] {
            return block_template->waitNext(options);
        }).then([context = kj::mv(context), worker_queue = m_worker_queue](std::unique_ptr<interfaces::BlockTemplate> result) mutable {
            auto output{context.getResults().initResult()};
            if (result) {
                output.setValue(messages::BlockTemplate::Client{kj::heap<BlockTemplateServer>(std::move(result), worker_queue)});
            } else {
                output.setNone({});
            }
        });
    }

    kj::Promise<void> interruptWait(InterruptWaitContext) override
    {
        m_block_template->interruptWait();
        return kj::READY_NOW;
    }

private:
    std::shared_ptr<interfaces::BlockTemplate> m_block_template;
    std::shared_ptr<WorkerQueue> m_worker_queue;
};

class MiningServer final : public messages::Mining::Server
{
public:
    MiningServer(std::unique_ptr<interfaces::Mining> mining, std::shared_ptr<WorkerQueue> worker_queue)
        : m_mining{std::move(mining)}, m_worker_queue{std::move(worker_queue)}
    {
    }

protected:
    kj::Promise<void> isTestChain(IsTestChainContext context) override
    {
        context.getResults().setResult(m_mining->isTestChain());
        return kj::READY_NOW;
    }

    kj::Promise<void> isInitialBlockDownload(IsInitialBlockDownloadContext context) override
    {
        context.getResults().setResult(m_mining->isInitialBlockDownload());
        return kj::READY_NOW;
    }

    kj::Promise<void> getTip(GetTipContext context) override
    {
        BuildOptionalBlockRef(context.getResults().initResult(), m_mining->getTip());
        return kj::READY_NOW;
    }

    kj::Promise<void> waitTipChanged(WaitTipChangedContext context) override
    {
        auto params{context.getParams()};
        uint256 current_tip{ReadUint256(params.getCurrentTip())};
        MillisecondsDouble timeout{params.getTimeout()};
        return m_worker_queue->post([mining = m_mining, current_tip, timeout] {
            return mining->waitTipChanged(current_tip, timeout);
        }).then([context = kj::mv(context)](std::optional<interfaces::BlockRef> result) mutable {
            BuildOptionalBlockRef(context.getResults().initResult(), result);
        });
    }

    kj::Promise<void> createNewBlock(CreateNewBlockContext context) override
    {
        auto params{context.getParams()};
        node::BlockCreateOptions options{ReadBlockCreateOptions(params.getOptions())};
        const bool cooldown{params.getCooldown()};
        return m_worker_queue->post([mining = m_mining, options, cooldown] {
            return mining->createNewBlock(options, cooldown);
        }).then([context = kj::mv(context), worker_queue = m_worker_queue](std::unique_ptr<interfaces::BlockTemplate> result) mutable {
            auto output{context.getResults().initResult()};
            if (result) {
                output.setValue(messages::BlockTemplate::Client{kj::heap<BlockTemplateServer>(std::move(result), worker_queue)});
            } else {
                output.setNone({});
            }
        });
    }

    kj::Promise<void> checkBlock(CheckBlockContext context) override
    {
        auto params{context.getParams()};
        CBlock block{ReadData<CBlock>(params.getBlock())};
        node::BlockCheckOptions options{ReadBlockCheckOptions(params.getOptions())};
        return m_worker_queue->post([mining = m_mining, block = std::move(block), options]() mutable {
            std::string reason;
            std::string debug;
            const bool result{mining->checkBlock(block, options, reason, debug)};
            return std::make_tuple(result, std::move(reason), std::move(debug));
        }).then([context = kj::mv(context)](std::tuple<bool, std::string, std::string> result) mutable {
            context.getResults().setResult(std::get<0>(result));
            context.getResults().setReason(std::get<1>(result));
            context.getResults().setDebug(std::get<2>(result));
        });
    }

    kj::Promise<void> interrupt(InterruptContext) override
    {
        m_mining->interrupt();
        return kj::READY_NOW;
    }

private:
    std::shared_ptr<interfaces::Mining> m_mining;
    std::shared_ptr<WorkerQueue> m_worker_queue;
};

class InitServer final : public messages::Init::Server
{
public:
    InitServer(interfaces::Init& init, std::shared_ptr<WorkerQueue> worker_queue)
        : m_init{init}, m_worker_queue{std::move(worker_queue)}
    {
    }

protected:
    kj::Promise<void> makeEcho(MakeEchoContext context) override
    {
        auto echo{m_init.makeEcho()};
        KJ_REQUIRE(echo != nullptr, "Init::makeEcho returned null.");
        context.getResults().setResult(messages::Echo::Client{kj::heap<EchoServer>(std::move(echo))});
        return kj::READY_NOW;
    }

    kj::Promise<void> makeRpc(MakeRpcContext context) override
    {
        auto rpc{m_init.makeRpc()};
        KJ_REQUIRE(rpc != nullptr, "Init::makeRpc returned null.");
        context.getResults().setResult(messages::Rpc::Client{kj::heap<RpcServer>(std::move(rpc), m_worker_queue)});
        return kj::READY_NOW;
    }

    kj::Promise<void> makeMining(MakeMiningContext context) override
    {
        auto mining{m_init.makeMining()};
        KJ_REQUIRE(mining != nullptr, "Init::makeMining returned null.");
        context.getResults().setResult(messages::Mining::Client{kj::heap<MiningServer>(std::move(mining), m_worker_queue)});
        return kj::READY_NOW;
    }

private:
    interfaces::Init& m_init;
    std::shared_ptr<WorkerQueue> m_worker_queue;
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

class RpcClient final : public interfaces::Rpc, public CleanupHandler
{
public:
    RpcClient(messages::Rpc::Client rpc, std::shared_ptr<ClientContext> context)
        : m_rpc{kj::mv(rpc)}, m_context{std::move(context)}
    {
    }
    ~RpcClient() noexcept override = default;

    UniValue executeRpc(UniValue request, std::string uri, std::string user) override
    {
        std::lock_guard<std::mutex> lock{m_context->mutex};
        auto rpc_request{m_rpc.executeRpcRequest()};
        rpc_request.setRequest(WriteUniValue(request));
        rpc_request.setUri(uri);
        rpc_request.setUser(user);
        auto response{rpc_request.send().wait(m_context->connection->waitScope())};
        return ReadUniValue(response.getResult().cStr());
    }

private:
    messages::Rpc::Client m_rpc;
    std::shared_ptr<ClientContext> m_context;
};

class BlockTemplateClient final : public interfaces::BlockTemplate, public CleanupHandler
{
public:
    BlockTemplateClient(messages::BlockTemplate::Client block_template, std::shared_ptr<ClientContext> context)
        : m_block_template{kj::mv(block_template)}, m_context{std::move(context)}
    {
    }
    ~BlockTemplateClient() noexcept override = default;

    CBlockHeader getBlockHeader() override
    {
        std::lock_guard<std::mutex> lock{m_context->mutex};
        auto response{m_block_template.getBlockHeaderRequest().send().wait(m_context->connection->waitScope())};
        return ReadData<CBlockHeader>(response.getResult());
    }

    CBlock getBlock() override
    {
        std::lock_guard<std::mutex> lock{m_context->mutex};
        auto response{m_block_template.getBlockRequest().send().wait(m_context->connection->waitScope())};
        return ReadData<CBlock>(response.getResult());
    }

    std::vector<CAmount> getTxFees() override
    {
        std::lock_guard<std::mutex> lock{m_context->mutex};
        auto response{m_block_template.getTxFeesRequest().send().wait(m_context->connection->waitScope())};
        std::vector<CAmount> result;
        for (auto fee : response.getResult()) {
            result.push_back(fee);
        }
        return result;
    }

    std::vector<int64_t> getTxSigops() override
    {
        std::lock_guard<std::mutex> lock{m_context->mutex};
        auto response{m_block_template.getTxSigopsRequest().send().wait(m_context->connection->waitScope())};
        std::vector<int64_t> result;
        for (auto sigops : response.getResult()) {
            result.push_back(sigops);
        }
        return result;
    }

    node::CoinbaseTx getCoinbaseTx() override
    {
        std::lock_guard<std::mutex> lock{m_context->mutex};
        auto response{m_block_template.getCoinbaseTxRequest().send().wait(m_context->connection->waitScope())};
        return ReadCoinbaseTx(response.getResult());
    }

    std::vector<uint256> getCoinbaseMerklePath() override
    {
        std::lock_guard<std::mutex> lock{m_context->mutex};
        auto response{m_block_template.getCoinbaseMerklePathRequest().send().wait(m_context->connection->waitScope())};
        std::vector<uint256> result;
        for (auto hash : response.getResult()) {
            result.push_back(ReadUint256(hash));
        }
        return result;
    }

    bool submitSolution(uint32_t version, uint32_t timestamp, uint32_t nonce, CTransactionRef coinbase) override
    {
        std::lock_guard<std::mutex> lock{m_context->mutex};
        auto request{m_block_template.submitSolutionRequest()};
        request.setVersion(version);
        request.setTimestamp(timestamp);
        request.setNonce(nonce);
        const std::vector<unsigned char> serialized_coinbase{SerializeData(*coinbase)};
        request.setCoinbase(MakeDataReader(serialized_coinbase));
        auto response{request.send().wait(m_context->connection->waitScope())};
        return response.getResult();
    }

    std::unique_ptr<interfaces::BlockTemplate> waitNext(node::BlockWaitOptions options) override
    {
        std::lock_guard<std::mutex> lock{m_context->mutex};
        auto request{m_block_template.waitNextRequest()};
        BuildBlockWaitOptions(request.initOptions(), options);
        auto response{request.send().wait(m_context->connection->waitScope())};
        return readOptionalBlockTemplate(response.getResult());
    }

    void interruptWait() override
    {
        std::lock_guard<std::mutex> lock{m_context->mutex};
        m_block_template.interruptWaitRequest().send().wait(m_context->connection->waitScope());
    }

private:
    std::unique_ptr<interfaces::BlockTemplate> readOptionalBlockTemplate(messages::OptionalBlockTemplate::Reader input)
    {
        switch (input.which()) {
        case messages::OptionalBlockTemplate::NONE:
            return nullptr;
        case messages::OptionalBlockTemplate::VALUE:
            return std::make_unique<BlockTemplateClient>(input.getValue(), m_context);
        }
        assert(false);
        return nullptr;
    }

    messages::BlockTemplate::Client m_block_template;
    std::shared_ptr<ClientContext> m_context;
};

class MiningClient final : public interfaces::Mining, public CleanupHandler
{
public:
    MiningClient(messages::Mining::Client mining, std::shared_ptr<ClientContext> context)
        : m_mining{kj::mv(mining)}, m_context{std::move(context)}
    {
    }
    ~MiningClient() noexcept override = default;

    bool isTestChain() override
    {
        std::lock_guard<std::mutex> lock{m_context->mutex};
        auto response{m_mining.isTestChainRequest().send().wait(m_context->connection->waitScope())};
        return response.getResult();
    }

    bool isInitialBlockDownload() override
    {
        std::lock_guard<std::mutex> lock{m_context->mutex};
        auto response{m_mining.isInitialBlockDownloadRequest().send().wait(m_context->connection->waitScope())};
        return response.getResult();
    }

    std::optional<interfaces::BlockRef> getTip() override
    {
        std::lock_guard<std::mutex> lock{m_context->mutex};
        auto response{m_mining.getTipRequest().send().wait(m_context->connection->waitScope())};
        return ReadOptionalBlockRef(response.getResult());
    }

    std::optional<interfaces::BlockRef> waitTipChanged(uint256 current_tip, MillisecondsDouble timeout) override
    {
        std::lock_guard<std::mutex> lock{m_context->mutex};
        auto request{m_mining.waitTipChangedRequest()};
        request.setCurrentTip(MakeDataReader({current_tip.data(), current_tip.size()}));
        request.setTimeout(timeout.count());
        auto response{request.send().wait(m_context->connection->waitScope())};
        return ReadOptionalBlockRef(response.getResult());
    }

    std::unique_ptr<interfaces::BlockTemplate> createNewBlock(const node::BlockCreateOptions& options, bool cooldown) override
    {
        std::lock_guard<std::mutex> lock{m_context->mutex};
        auto request{m_mining.createNewBlockRequest()};
        BuildBlockCreateOptions(request.initOptions(), options);
        request.setCooldown(cooldown);
        auto response{request.send().wait(m_context->connection->waitScope())};
        return readOptionalBlockTemplate(response.getResult());
    }

    void interrupt() override
    {
        std::lock_guard<std::mutex> lock{m_context->mutex};
        m_mining.interruptRequest().send().wait(m_context->connection->waitScope());
    }

    bool checkBlock(const CBlock& block, const node::BlockCheckOptions& options, std::string& reason, std::string& debug) override
    {
        std::lock_guard<std::mutex> lock{m_context->mutex};
        auto request{m_mining.checkBlockRequest()};
        const std::vector<unsigned char> serialized_block{SerializeData(block)};
        request.setBlock(MakeDataReader(serialized_block));
        BuildBlockCheckOptions(request.initOptions(), options);
        auto response{request.send().wait(m_context->connection->waitScope())};
        reason = response.getReason().cStr();
        debug = response.getDebug().cStr();
        return response.getResult();
    }

private:
    std::unique_ptr<interfaces::BlockTemplate> readOptionalBlockTemplate(messages::OptionalBlockTemplate::Reader input)
    {
        switch (input.which()) {
        case messages::OptionalBlockTemplate::NONE:
            return nullptr;
        case messages::OptionalBlockTemplate::VALUE:
            return std::make_unique<BlockTemplateClient>(input.getValue(), m_context);
        }
        assert(false);
        return nullptr;
    }

    messages::Mining::Client m_mining;
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

    std::unique_ptr<interfaces::Rpc> makeRpc() override
    {
        std::lock_guard<std::mutex> lock{m_context->mutex};
        auto response{m_context->connection->init().makeRpcRequest().send().wait(m_context->connection->waitScope())};
        return std::make_unique<RpcClient>(response.getResult(), m_context);
    }

    std::unique_ptr<interfaces::Mining> makeMining() override
    {
        std::lock_guard<std::mutex> lock{m_context->mutex};
        auto response{m_context->connection->init().makeMiningRequest().send().wait(m_context->connection->waitScope())};
        return std::make_unique<MiningClient>(response.getResult(), m_context);
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
        } else if (type == typeid(interfaces::Rpc)) {
            handler = dynamic_cast<CleanupHandler*>(static_cast<interfaces::Rpc*>(iface));
        } else if (type == typeid(interfaces::Mining)) {
            handler = dynamic_cast<CleanupHandler*>(static_cast<interfaces::Mining*>(iface));
        } else if (type == typeid(interfaces::BlockTemplate)) {
            handler = dynamic_cast<CleanupHandler*>(static_cast<interfaces::BlockTemplate*>(iface));
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
    auto worker_queue{std::make_shared<WorkerQueue>("ipc-worker")};
    ::capnp::Capability::Client bootstrap{kj::heap<InitServer>(init, worker_queue)};
    auto rpc_system{::capnp::makeRpcServer(network, kj::mv(bootstrap))};
    (void)rpc_system;
    if (ready_fn) ready_fn();
    network.onDisconnect().wait(io_context.waitScope);
}

std::unique_ptr<Protocol> MakeCapnpProtocol() { return std::make_unique<CapnpProtocol>(); }
} // namespace capnp
} // namespace ipc
