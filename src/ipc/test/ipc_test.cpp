// Copyright (c) 2023-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <interfaces/init.h>
#include <interfaces/types.h>
#include <ipc/capnp/conversions.h>
#include <ipc/capnp/mining.capnp.h>
#include <ipc/capnp/protocol.h>
#include <ipc/capnp/worker_queue.h>
#include <ipc/process.h>
#include <ipc/protocol.h>
#include <logging.h>
#include <mp/proxy-types.h>
#include <ipc/test/ipc_test.capnp.h>
#include <ipc/test/ipc_test.capnp.proxy.h>
#include <ipc/test/ipc_test.h>
#include <tinyformat.h>
#include <validation.h>

#include <capnp/message.h>

#include <atomic>
#include <future>
#include <string>
#include <thread>
#include <kj/common.h>
#include <kj/memory.h>
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
};

//! Test native Cap'n Proto Init, Echo, and Rpc calls over a socketpair.
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

//! Unit test that tests execution of IPC calls without actually creating a
//! separate process. This test is primarily intended to verify behavior of type
//! conversion code that converts C++ objects to Cap'n Proto messages and vice
//! versa.
//!
//! The test creates a thread which creates a FooImplementation object (defined
//! in ipc_test.h) and a two-way pipe accepting IPC requests which call methods
//! on the object through FooInterface (defined in ipc_test.capnp).
void IpcPipeTest()
{
    // Setup: create FooImplementation object and listen for FooInterface requests
    std::promise<std::unique_ptr<mp::ProxyClient<gen::FooInterface>>> foo_promise;
    std::thread thread([&]() {
        mp::EventLoop loop("IpcPipeTest", [](bool raise, const std::string& log) { LogInfo("LOG%i: %s", raise, log); });
        auto pipe = loop.m_io_context.provider->newTwoWayPipe();

        auto connection_client = std::make_unique<mp::Connection>(loop, kj::mv(pipe.ends[0]));
        auto foo_client = std::make_unique<mp::ProxyClient<gen::FooInterface>>(
            connection_client->m_rpc_system->bootstrap(mp::ServerVatId().vat_id).castAs<gen::FooInterface>(),
            connection_client.get(), /* destroy_connection= */ true);
        (void)connection_client.release();
        foo_promise.set_value(std::move(foo_client));

        auto connection_server = std::make_unique<mp::Connection>(loop, kj::mv(pipe.ends[1]), [&](mp::Connection& connection) {
            auto foo_server = kj::heap<mp::ProxyServer<gen::FooInterface>>(std::make_shared<FooImplementation>(), connection);
            return capnp::Capability::Client(kj::mv(foo_server));
        });
        connection_server->onDisconnect([&] { connection_server.reset(); });
        loop.loop();
    });
    std::unique_ptr<mp::ProxyClient<gen::FooInterface>> foo{foo_promise.get_future().get()};

    // Test: make sure arguments were sent and return value is received
    BOOST_CHECK_EQUAL(foo->add(1, 2), 3);

    COutPoint txout1{Txid::FromUint256(uint256{100}), 200};
    COutPoint txout2{foo->passOutPoint(txout1)};
    BOOST_CHECK(txout1 == txout2);

    UniValue uni1{UniValue::VOBJ};
    uni1.pushKV("i", 1);
    uni1.pushKV("s", "two");
    UniValue uni2{foo->passUniValue(uni1)};
    BOOST_CHECK_EQUAL(uni1.write(), uni2.write());

    CMutableTransaction mtx;
    mtx.version = 2;
    mtx.nLockTime = 3;
    mtx.vin.emplace_back(txout1);
    mtx.vout.emplace_back(COIN, CScript());
    CTransactionRef tx1{MakeTransactionRef(mtx)};
    CTransactionRef tx2{foo->passTransaction(tx1)};
    BOOST_CHECK(*Assert(tx1) == *Assert(tx2));

    std::vector<char> vec1{'H', 'e', 'l', 'l', 'o'};
    std::vector<char> vec2{foo->passVectorChar(vec1)};
    BOOST_CHECK_EQUAL(std::string_view(vec1.begin(), vec1.end()), std::string_view(vec2.begin(), vec2.end()));

    auto script1{CScript() << OP_11};
    auto script2{foo->passScript(script1)};
    BOOST_CHECK_EQUAL(HexStr(script1), HexStr(script2));

    // Test cleanup: disconnect and join thread
    foo.reset();
    thread.join();
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
    std::unique_ptr<interfaces::Init> remote_init{protocol->connect(fds[1], "test-connect")};
    std::unique_ptr<interfaces::Echo> remote_echo{remote_init->makeEcho()};
    BOOST_CHECK_EQUAL(remote_echo->echo("echo test"), "echo test");
    remote_echo.reset();
    remote_init.reset();
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
        std::unique_ptr<interfaces::Init> remote_init{protocol->connect(connect_fd, "test-connect")};
        std::unique_ptr<interfaces::Echo> remote_echo{remote_init->makeEcho()};
        BOOST_CHECK_EQUAL(remote_echo->echo("echo test"), "echo test");
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
