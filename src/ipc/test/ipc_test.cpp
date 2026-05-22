// Copyright (c) 2023-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <interfaces/init.h>
#include <interfaces/types.h>
#include <ipc/capnp/conversions.h>
#include <ipc/capnp/event_loop.h>
#include <ipc/capnp/mining.capnp.h>
#include <ipc/capnp/protocol.h>
#include <ipc/capnp/worker_queue.h>
#include <ipc/process.h>
#include <ipc/protocol.h>
#include <ipc/test/ipc_test.h>
#include <tinyformat.h>
#include <validation.h>

#include <capnp/message.h>

#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <thread>
#include <kj/async-io.h>
#include <kj/common.h>
#include <kj/time.h>
#include <kj/test.h>
#include <stdexcept>

#include <boost/test/unit_test.hpp>

static_assert(ipc::capnp::messages::MAX_MONEY == MAX_MONEY);
static_assert(ipc::capnp::messages::MAX_DOUBLE == std::numeric_limits<double>::max());
static_assert(ipc::capnp::messages::DEFAULT_BLOCK_RESERVED_WEIGHT == DEFAULT_BLOCK_RESERVED_WEIGHT);
static_assert(ipc::capnp::messages::DEFAULT_COINBASE_OUTPUT_MAX_ADDITIONAL_SIGOPS == DEFAULT_COINBASE_OUTPUT_MAX_ADDITIONAL_SIGOPS);

void IpcConversionTest()
{
    using namespace ipc::capnp;

    {
        interfaces::BlockRef block_ref{uint256{123}, 456};
        ::capnp::MallocMessageBuilder builder;
        auto output{builder.initRoot<messages::BlockRef>()};
        BuildBlockRef(output, block_ref);

        interfaces::BlockRef input{ReadBlockRef(output.asReader())};
        BOOST_CHECK_EQUAL(input.hash.ToString(), block_ref.hash.ToString());
        BOOST_CHECK_EQUAL(input.height, block_ref.height);
    }

    {
        std::optional<interfaces::BlockRef> block_ref{interfaces::BlockRef{uint256{234}, 567}};
        ::capnp::MallocMessageBuilder builder;
        auto output{builder.initRoot<messages::OptionalBlockRef>()};
        BuildOptionalBlockRef(output, block_ref);

        std::optional<interfaces::BlockRef> input{ReadOptionalBlockRef(output.asReader())};
        BOOST_REQUIRE(input);
        BOOST_CHECK_EQUAL(input->hash.ToString(), block_ref->hash.ToString());
        BOOST_CHECK_EQUAL(input->height, block_ref->height);
    }

    {
        ::capnp::MallocMessageBuilder builder;
        auto output{builder.initRoot<messages::OptionalBlockRef>()};
        BuildOptionalBlockRef(output, std::nullopt);
        BOOST_CHECK(!ReadOptionalBlockRef(output.asReader()));
    }

    {
        node::BlockCreateOptions options;
        options.use_mempool = false;
        options.block_reserved_weight = 9000;
        options.coinbase_output_max_additional_sigops = 123;

        ::capnp::MallocMessageBuilder builder;
        auto output{builder.initRoot<messages::BlockCreateOptions>()};
        BuildBlockCreateOptions(output, options);

        node::BlockCreateOptions input{ReadBlockCreateOptions(output.asReader())};
        BOOST_CHECK_EQUAL(input.use_mempool, options.use_mempool);
        BOOST_REQUIRE(input.block_reserved_weight);
        BOOST_CHECK_EQUAL(*input.block_reserved_weight, *options.block_reserved_weight);
        BOOST_CHECK_EQUAL(input.coinbase_output_max_additional_sigops, options.coinbase_output_max_additional_sigops);
    }

    {
        node::BlockWaitOptions options;
        options.timeout = MillisecondsDouble{2500};
        options.fee_threshold = 12345;

        ::capnp::MallocMessageBuilder builder;
        auto output{builder.initRoot<messages::BlockWaitOptions>()};
        BuildBlockWaitOptions(output, options);

        node::BlockWaitOptions input{ReadBlockWaitOptions(output.asReader())};
        BOOST_CHECK_EQUAL(input.timeout.count(), options.timeout.count());
        BOOST_CHECK_EQUAL(input.fee_threshold, options.fee_threshold);
    }

    {
        node::BlockCheckOptions options;
        options.check_merkle_root = false;
        options.check_pow = false;

        ::capnp::MallocMessageBuilder builder;
        auto output{builder.initRoot<messages::BlockCheckOptions>()};
        BuildBlockCheckOptions(output, options);

        node::BlockCheckOptions input{ReadBlockCheckOptions(output.asReader())};
        BOOST_CHECK_EQUAL(input.check_merkle_root, options.check_merkle_root);
        BOOST_CHECK_EQUAL(input.check_pow, options.check_pow);
    }

    {
        node::CoinbaseTx tx;
        tx.version = 2;
        tx.sequence = 3;
        tx.script_sig_prefix = CScript() << OP_1 << std::vector<unsigned char>{0x04, 0x05};
        tx.witness = uint256{45};
        tx.block_reward_remaining = 50 * COIN;
        tx.required_outputs.emplace_back(1000, CScript() << OP_TRUE);
        tx.lock_time = 4;

        ::capnp::MallocMessageBuilder builder;
        auto output{builder.initRoot<messages::CoinbaseTx>()};
        BuildCoinbaseTx(output, tx);

        node::CoinbaseTx input{ReadCoinbaseTx(output.asReader())};
        BOOST_CHECK_EQUAL(input.version, tx.version);
        BOOST_CHECK_EQUAL(input.sequence, tx.sequence);
        BOOST_CHECK(input.script_sig_prefix == tx.script_sig_prefix);
        BOOST_REQUIRE(input.witness);
        BOOST_CHECK_EQUAL(input.witness->ToString(), tx.witness->ToString());
        BOOST_CHECK_EQUAL(input.block_reward_remaining, tx.block_reward_remaining);
        BOOST_REQUIRE_EQUAL(input.required_outputs.size(), tx.required_outputs.size());
        BOOST_CHECK(input.required_outputs[0] == tx.required_outputs[0]);
        BOOST_CHECK_EQUAL(input.lock_time, tx.lock_time);
    }

    {
        CMutableTransaction mtx;
        mtx.version = 2;
        mtx.nLockTime = 3;
        mtx.vin.emplace_back(COutPoint{Txid::FromUint256(uint256{56}), 7});
        mtx.vout.emplace_back(8 * COIN, CScript() << OP_2);
        CTransactionRef tx{MakeTransactionRef(mtx)};

        std::vector<unsigned char> serialized{ipc::capnp::SerializeData(*tx)};
        CTransactionRef input{ReadTransaction(MakeDataReader(serialized))};
        BOOST_CHECK(*input == *tx);
    }

    {
        UniValue value{UniValue::VOBJ};
        value.pushKV("number", 1);
        value.pushKV("string", "two");

        std::string json{WriteUniValue(value)};
        UniValue input{ReadUniValue(json)};
        BOOST_CHECK_EQUAL(input.write(), value.write());
    }
}

void IpcWorkerQueueTest()
{
    kj::EventLoop loop;
    kj::WaitScope wait_scope{loop};
    ipc::capnp::WorkerQueue queue{"ipc-test-worker"};

    {
        std::vector<int> order;
        auto first{queue.post([&] {
            order.push_back(1);
            return 10;
        })};
        auto second{queue.post([&] {
            order.push_back(2);
            return 20;
        })};

        BOOST_CHECK_EQUAL(first.wait(wait_scope), 10);
        BOOST_CHECK_EQUAL(second.wait(wait_scope), 20);
        BOOST_REQUIRE_EQUAL(order.size(), 2);
        BOOST_CHECK_EQUAL(order[0], 1);
        BOOST_CHECK_EQUAL(order[1], 2);
    }

    {
        std::promise<void> started;
        auto started_future{started.get_future()};
        std::promise<void> release;
        auto release_future{release.get_future().share()};
        std::atomic<bool> second_started{false};

        auto blocking{queue.post([&] {
            started.set_value();
            release_future.wait();
            return 1;
        })};
        started_future.wait();

        auto queued{queue.post([&] {
            second_started = true;
            return 2;
        })};
        BOOST_CHECK(!second_started);

        release.set_value();
        BOOST_CHECK_EQUAL(blocking.wait(wait_scope), 1);
        BOOST_CHECK_EQUAL(queued.wait(wait_scope), 2);
        BOOST_CHECK(second_started);
    }

    {
        auto failed{queue.post([]() -> int {
            throw std::runtime_error{"queue failure"};
        })};

        BOOST_CHECK_EXCEPTION(failed.wait(wait_scope), kj::Exception, [](const kj::Exception& e) {
            return std::string{e.getDescription().cStr()}.find("queue failure") != std::string::npos;
        });
    }
}

void IpcEventLoopDispatcherTest()
{
    auto io_context{kj::setupAsyncIo()};
    auto dispatcher{ipc::capnp::EventLoopDispatcher::CurrentThread()};
    const std::thread::id event_loop_thread{std::this_thread::get_id()};

    {
        bool ran{false};
        dispatcher->execute([&] {
            BOOST_CHECK_EQUAL(std::this_thread::get_id(), event_loop_thread);
            ran = true;
        });
        BOOST_CHECK(ran);
    }

    {
        std::atomic<bool> ran{false};
        std::thread::id callback_thread;
        std::thread worker{[&] {
            dispatcher->execute([&] {
                callback_thread = std::this_thread::get_id();
                ran.store(true, std::memory_order_release);
            });
        }};

        const auto deadline{std::chrono::steady_clock::now() + std::chrono::seconds{5}};
        bool timed_out{false};
        while (!ran.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= deadline) {
                timed_out = true;
                break;
            }
            io_context.provider->getTimer().afterDelay(1 * kj::MILLISECONDS).wait(io_context.waitScope);
        }
        worker.join();
        BOOST_REQUIRE(!timed_out);
        BOOST_CHECK_EQUAL(callback_thread, event_loop_thread);
    }
}

//! Remote init class.
class TestInit : public interfaces::Init
{
public:
    std::unique_ptr<interfaces::Echo> makeEcho() override { return interfaces::MakeEcho(); }
    std::unique_ptr<interfaces::Rpc> makeRpc() override
    {
        class Rpc final : public interfaces::Rpc
        {
        public:
            UniValue executeRpc(UniValue request, std::string uri, std::string user) override
            {
                UniValue result{UniValue::VOBJ};
                result.pushKV("request", request);
                result.pushKV("uri", uri);
                result.pushKV("user", user);
                return result;
            }
        };
        return std::make_unique<Rpc>();
    }
    std::unique_ptr<interfaces::Mining> makeMining() override
    {
        class BlockTemplate final : public interfaces::BlockTemplate
        {
        public:
            CBlockHeader getBlockHeader() override
            {
                CBlockHeader header;
                header.nVersion = 7;
                return header;
            }

            CBlock getBlock() override
            {
                CBlock block;
                block.nVersion = 8;
                return block;
            }

            std::vector<CAmount> getTxFees() override { return {1, 2}; }
            std::vector<int64_t> getTxSigops() override { return {3, 4}; }

            node::CoinbaseTx getCoinbaseTx() override
            {
                node::CoinbaseTx tx;
                tx.version = 2;
                tx.sequence = 3;
                tx.script_sig_prefix = CScript() << OP_1;
                tx.witness = uint256{4};
                tx.block_reward_remaining = 5;
                tx.required_outputs.emplace_back(6, CScript() << OP_TRUE);
                tx.lock_time = 7;
                return tx;
            }

            std::vector<uint256> getCoinbaseMerklePath() override { return {uint256{9}}; }

            bool submitSolution(uint32_t version, uint32_t timestamp, uint32_t nonce, CTransactionRef) override
            {
                return version == 1 && timestamp == 2 && nonce == 3;
            }

            std::unique_ptr<interfaces::BlockTemplate> waitNext(node::BlockWaitOptions options) override
            {
                return options.fee_threshold == 7 ? std::make_unique<BlockTemplate>() : nullptr;
            }

            void interruptWait() override {}
        };

        class Mining final : public interfaces::Mining
        {
        public:
            bool isTestChain() override { return true; }
            bool isInitialBlockDownload() override { return false; }
            std::optional<interfaces::BlockRef> getTip() override { return interfaces::BlockRef{uint256{123}, 456}; }

            std::optional<interfaces::BlockRef> waitTipChanged(uint256, MillisecondsDouble) override
            {
                return interfaces::BlockRef{uint256{234}, 567};
            }

            std::unique_ptr<interfaces::BlockTemplate> createNewBlock(const node::BlockCreateOptions&, bool) override
            {
                return std::make_unique<BlockTemplate>();
            }

            void interrupt() override {}

            bool checkBlock(const CBlock& block, const node::BlockCheckOptions&, std::string& reason, std::string& debug) override
            {
                reason = "reason";
                debug = "debug";
                return block.nVersion == 7;
            }
        };
        return std::make_unique<Mining>();
    }
};

//! Test native Cap'n Proto Init, Echo, Rpc, Mining, and BlockTemplate calls over a socketpair.
void IpcNativeSocketPairTest()
{
    int fds[2];
    BOOST_CHECK_EQUAL(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    std::unique_ptr<interfaces::Init> init{std::make_unique<TestInit>()};
    std::promise<void> promise;
    std::thread thread([&]() {
        ipc::capnp::ServeNative(fds[0], *init, [&] { promise.set_value(); });
    });
    promise.get_future().wait();

    {
        std::unique_ptr<ipc::capnp::NativeConnection> connection{ipc::capnp::ConnectNative(fds[1])};
        auto remote_init{connection->init()};
        auto make_echo_request{remote_init.makeEchoRequest()};
        auto make_echo_response{make_echo_request.send().wait(connection->waitScope())};
        BOOST_CHECK(make_echo_response.hasResult());

        auto remote_echo{make_echo_response.getResult()};
        auto echo_request{remote_echo.echoRequest()};
        echo_request.setEcho("echo test");
        auto echo_response{echo_request.send().wait(connection->waitScope())};
        BOOST_CHECK_EQUAL(echo_response.getResult().cStr(), "echo test");

        UniValue rpc_input{UniValue::VOBJ};
        rpc_input.pushKV("method", "test");
        rpc_input.pushKV("params", UniValue{UniValue::VARR});

        auto make_rpc_request{remote_init.makeRpcRequest()};
        auto make_rpc_response{make_rpc_request.send().wait(connection->waitScope())};
        BOOST_CHECK(make_rpc_response.hasResult());

        auto remote_rpc{make_rpc_response.getResult()};
        auto rpc_request{remote_rpc.executeRpcRequest()};
        rpc_request.setRequest(ipc::capnp::WriteUniValue(rpc_input));
        rpc_request.setUri("/test");
        rpc_request.setUser("user");
        auto rpc_response{rpc_request.send().wait(connection->waitScope())};
        UniValue rpc_output{ipc::capnp::ReadUniValue(rpc_response.getResult().cStr())};
        BOOST_CHECK_EQUAL(rpc_output["request"]["method"].get_str(), "test");
        BOOST_CHECK_EQUAL(rpc_output["uri"].get_str(), "/test");
        BOOST_CHECK_EQUAL(rpc_output["user"].get_str(), "user");

        auto make_mining_response{remote_init.makeMiningRequest().send().wait(connection->waitScope())};
        BOOST_CHECK(make_mining_response.hasResult());
        auto remote_mining{make_mining_response.getResult()};

        auto is_test_chain_response{remote_mining.isTestChainRequest().send().wait(connection->waitScope())};
        BOOST_CHECK(is_test_chain_response.getResult());

        auto get_tip_response{remote_mining.getTipRequest().send().wait(connection->waitScope())};
        std::optional<interfaces::BlockRef> tip{ipc::capnp::ReadOptionalBlockRef(get_tip_response.getResult())};
        BOOST_REQUIRE(tip);
        BOOST_CHECK_EQUAL(tip->height, 456);

        auto create_block_request{remote_mining.createNewBlockRequest()};
        node::BlockCreateOptions create_options;
        create_options.use_mempool = false;
        ipc::capnp::BuildBlockCreateOptions(create_block_request.initOptions(), create_options);
        auto create_block_response{create_block_request.send().wait(connection->waitScope())};
        auto optional_template{create_block_response.getResult()};
        BOOST_REQUIRE(optional_template.which() == ipc::capnp::messages::OptionalBlockTemplate::VALUE);
        auto remote_template{optional_template.getValue()};

        auto header_response{remote_template.getBlockHeaderRequest().send().wait(connection->waitScope())};
        CBlockHeader header{ipc::capnp::ReadData<CBlockHeader>(header_response.getResult())};
        BOOST_CHECK_EQUAL(header.nVersion, 7);

        auto fees_response{remote_template.getTxFeesRequest().send().wait(connection->waitScope())};
        BOOST_REQUIRE_EQUAL(fees_response.getResult().size(), 2);
        BOOST_CHECK_EQUAL(fees_response.getResult()[0], 1);
        BOOST_CHECK_EQUAL(fees_response.getResult()[1], 2);

        auto check_block_request{remote_mining.checkBlockRequest()};
        CBlock block;
        block.nVersion = 7;
        const std::vector<unsigned char> serialized_block{ipc::capnp::SerializeData(block)};
        check_block_request.setBlock(ipc::capnp::MakeDataReader(serialized_block));
        ipc::capnp::BuildBlockCheckOptions(check_block_request.initOptions(), {});
        auto check_block_response{check_block_request.send().wait(connection->waitScope())};
        BOOST_CHECK(check_block_response.getResult());
        BOOST_CHECK_EQUAL(check_block_response.getReason().cStr(), "reason");
        BOOST_CHECK_EQUAL(check_block_response.getDebug().cStr(), "debug");
    }
    thread.join();
}

//! Generate a temporary path with temp_directory_path and mkstemp
static std::string TempPath(std::string_view pattern)
{
    std::string temp{fs::PathToString(fs::path{fs::temp_directory_path()} / fs::PathFromString(std::string{pattern}))};
    temp.push_back('\0');
    int fd{mkstemp(temp.data())};
    BOOST_CHECK_GE(fd, 0);
    BOOST_CHECK_EQUAL(close(fd), 0);
    temp.resize(temp.size() - 1);
    fs::remove(fs::PathFromString(temp));
    return temp;
}

//! Test ipc::Protocol connect() and serve() methods connecting over a socketpair.
void IpcSocketPairTest()
{
    int fds[2];
    BOOST_CHECK_EQUAL(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    std::unique_ptr<interfaces::Init> init{std::make_unique<TestInit>()};
    std::unique_ptr<ipc::Protocol> protocol{ipc::capnp::MakeCapnpProtocol()};
    std::promise<void> promise;
    std::thread thread([&]() {
        protocol->serve(fds[0], "test-serve", *init, [&] { promise.set_value(); });
    });
    promise.get_future().wait();
    std::unique_ptr<ipc::capnp::NativeConnection> connection{protocol->connect(fds[1], "test-connect")};
    auto remote_init{connection->init()};
    auto echo_response{remote_init.makeEchoRequest().send().wait(connection->waitScope())};
    auto remote_echo{echo_response.getResult()};
    auto echo_request{remote_echo.echoRequest()};
    echo_request.setEcho("echo test");
    BOOST_CHECK_EQUAL(echo_request.send().wait(connection->waitScope()).getResult().cStr(), "echo test");
    auto mining_response{remote_init.makeMiningRequest().send().wait(connection->waitScope())};
    auto remote_mining{mining_response.getResult()};
    BOOST_CHECK(remote_mining.isTestChainRequest().send().wait(connection->waitScope()).getResult());
    std::optional<interfaces::BlockRef> tip{ipc::capnp::ReadOptionalBlockRef(remote_mining.getTipRequest().send().wait(connection->waitScope()).getResult())};
    BOOST_REQUIRE(tip);
    BOOST_CHECK_EQUAL(tip->height, 456);
    connection.reset();
    thread.join();
}

//! Test ipc::Process bind() and connect() methods connecting over a unix socket.
void IpcSocketTest(const fs::path& datadir)
{
    std::unique_ptr<interfaces::Init> init{std::make_unique<TestInit>()};
    std::unique_ptr<ipc::Protocol> protocol{ipc::capnp::MakeCapnpProtocol()};
    std::unique_ptr<ipc::Process> process{ipc::MakeProcess()};

    std::string invalid_bind{"invalid:"};
    BOOST_CHECK_THROW(process->bind(datadir, "test_bitcoin", invalid_bind), std::invalid_argument);
    BOOST_CHECK_THROW(process->connect(datadir, "test_bitcoin", invalid_bind), std::invalid_argument);

    auto bind_and_listen{[&](const std::string& bind_address) {
        std::string address{bind_address};
        int serve_fd = process->bind(datadir, "test_bitcoin", address);
        BOOST_CHECK_GE(serve_fd, 0);
        BOOST_CHECK_EQUAL(address, bind_address);
        protocol->listen(serve_fd, "test-serve", *init);
    }};

    auto connect_and_test{[&](const std::string& connect_address) {
        std::string address{connect_address};
        int connect_fd{process->connect(datadir, "test_bitcoin", address)};
        BOOST_CHECK_EQUAL(address, connect_address);
        std::unique_ptr<ipc::capnp::NativeConnection> connection{protocol->connect(connect_fd, "test-connect")};
        auto echo_response{connection->init().makeEchoRequest().send().wait(connection->waitScope())};
        auto remote_echo{echo_response.getResult()};
        auto echo_request{remote_echo.echoRequest()};
        echo_request.setEcho("echo test");
        BOOST_CHECK_EQUAL(echo_request.send().wait(connection->waitScope()).getResult().cStr(), "echo test");
    }};

    // Need to specify explicit socket addresses outside the data directory, because the data
    // directory path is so long that the default socket address and any other
    // addresses in the data directory would fail with errors like:
    //   Address 'unix' path '"/tmp/test_common_Bitcoin Core/ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff/test_bitcoin.sock"' exceeded maximum socket path length
    std::vector<std::string> addresses{
        strprintf("unix:%s", TempPath("bitcoin_sock0_XXXXXX")),
        strprintf("unix:%s", TempPath("bitcoin_sock1_XXXXXX")),
    };

    // Bind and listen on multiple addresses
    for (const auto& address : addresses) {
        bind_and_listen(address);
    }

    // Connect and test each address multiple times.
    for (int i : {0, 1, 0, 0, 1}) {
        connect_and_test(addresses[i]);
    }
}
