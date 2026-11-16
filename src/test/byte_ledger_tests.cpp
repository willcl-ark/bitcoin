// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <core_memusage.h>
#include <kernel/effects.h>
#include <node/blockcache.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <test/util/setup_common.h>
#include <util/byte_ledger.h>

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

using util::ByteAccount;
using util::ByteLedger;
using Id = util::ByteAccountId;

BOOST_FIXTURE_TEST_SUITE(byte_ledger_tests, BasicTestingSetup)

namespace {

//! A block of about a kilobyte whose hash depends on `height` only.
std::shared_ptr<const CBlock> MakeBlock(int height)
{
    auto block{std::make_shared<CBlock>()};
    block->nNonce = height;
    CMutableTransaction tx;
    tx.vin.emplace_back();
    const std::vector<unsigned char> bytes(1000, 0x51);
    tx.vin[0].scriptSig = CScript(bytes.begin(), bytes.end());
    block->vtx.push_back(MakeTransactionRef(std::move(tx)));
    return block;
}

} // namespace

BOOST_AUTO_TEST_CASE(an_account_admits_up_to_its_limit)
{
    ByteAccount account;
    account.SetLimit(100);
    BOOST_CHECK(account.Admits());
    BOOST_CHECK(account.Admits(100));
    BOOST_CHECK(!account.Admits(101));

    account.Add(60);
    BOOST_CHECK(account.Admits(40));
    BOOST_CHECK(!account.Admits(41));

    // A holder may go over (one block is always in flight, whatever it weighs): nothing more is admitted
    // until it is back within the limit.
    account.Add(60);
    BOOST_CHECK_EQUAL(account.Bytes(), 120U);
    BOOST_CHECK(!account.Admits());
    account.Sub(20);
    BOOST_CHECK(account.Admits());
    BOOST_CHECK(!account.Admits(1));
}

BOOST_AUTO_TEST_CASE(an_account_without_a_limit_is_counted_and_admits_everything)
{
    ByteAccount account;
    BOOST_CHECK_EQUAL(account.Limit(), 0U);
    account.Add(size_t{1} << 40);
    BOOST_CHECK(account.Admits(size_t{1} << 40));
    BOOST_CHECK_EQUAL(account.Bytes(), size_t{1} << 40);
}

BOOST_AUTO_TEST_CASE(the_peak_is_the_most_an_account_held)
{
    ByteAccount account;
    account.Add(10);
    account.Add(30);
    account.Sub(35);
    account.Add(5);
    BOOST_CHECK_EQUAL(account.Bytes(), 10U);
    BOOST_CHECK_EQUAL(account.Peak(), 40U);
    account.Set(70);
    account.Set(0);
    BOOST_CHECK_EQUAL(account.Bytes(), 0U);
    BOOST_CHECK_EQUAL(account.Peak(), 70U);
}

BOOST_AUTO_TEST_CASE(the_ledger_lists_every_account_once)
{
    ByteLedger ledger;
    size_t expected{0};
    size_t next{1};
    for (const Id id : util::BYTE_ACCOUNTS) {
        BOOST_CHECK(*util::ByteAccountName(id) != '\0');
        BOOST_CHECK_EQUAL(ledger[id].Bytes(), 0U); // not reached under another name
        ledger[id].Add(next);
        expected += next;
        next *= 2;
    }
    BOOST_CHECK_EQUAL(ledger.Total(), expected);
    BOOST_CHECK_EQUAL(ledger[Id::ReceiveTasks].Bytes(), 1U);
    BOOST_CHECK_EQUAL(ledger[Id::NodeRegions].Bytes(), next / 2);
}

BOOST_AUTO_TEST_CASE(the_block_cache_counts_in_its_account)
{
    ByteLedger ledger;
    ByteAccount& account{ledger[Id::BlockCache]};
    const size_t one{node::BlockCache::Bytes(*MakeBlock(1))};
    node::BlockCache cache{3 * one + one / 2, &account}; // room for three
    BOOST_CHECK_EQUAL(account.Limit(), 3 * one + one / 2);

    for (const int height : {5, 3, 4, 6, 2}) cache.Insert(height, MakeBlock(height), /*tip_height=*/1);
    // Each insertion over the limit dropped the highest block.
    BOOST_CHECK_EQUAL(cache.Count(), 3U);
    BOOST_CHECK_EQUAL(account.Bytes(), 3 * one);
    BOOST_CHECK(account.Admits());
    BOOST_CHECK(!account.Admits(one));

    BOOST_CHECK(cache.Take(2, MakeBlock(2)->GetHash()));
    BOOST_CHECK_EQUAL(account.Bytes(), 2 * one);
    cache.Drop(3, MakeBlock(3)->GetHash());
    BOOST_CHECK_EQUAL(account.Bytes(), one);
    // The tip passes the last block: it leaves at the next insertion, which the tip has passed as well.
    cache.Insert(7, MakeBlock(7), /*tip_height=*/4);
    BOOST_CHECK_EQUAL(account.Bytes(), one);
    BOOST_CHECK(cache.Take(7, MakeBlock(7)->GetHash()));
    BOOST_CHECK_EQUAL(account.Bytes(), 0U);
    BOOST_CHECK_EQUAL(ledger.Total(), 0U);
}

BOOST_AUTO_TEST_CASE(the_executor_counts_its_backlog_in_its_account)
{
    ByteLedger ledger;
    ByteAccount& account{ledger[Id::UndoBacklog]};
    account.SetLimit(50);
    std::atomic<bool> release{false};
    kernel::EffectsExecutor exec{{}, &account};
    exec.Start();
    // The first effect blocks until released, holding everything behind it.
    exec.Enqueue([&] { while (!release) std::this_thread::sleep_for(std::chrono::milliseconds{1}); return true; }, {}, 10);
    for (int i = 0; i < 9; ++i) exec.Enqueue([] { return true; }, {}, 10);
    BOOST_CHECK_EQUAL(account.Bytes(), 100U);
    BOOST_CHECK_EQUAL(exec.PendingBytes(), 100U);
    BOOST_CHECK(!account.Admits());

    std::thread waiter{[&] { exec.WaitForBacklogBelow(account.Limit()); }};
    release = true;
    waiter.join();
    BOOST_CHECK(account.Admits());

    exec.Stop();
    BOOST_CHECK_EQUAL(account.Bytes(), 0U);
    BOOST_CHECK_EQUAL(account.Peak(), 100U);
}

BOOST_AUTO_TEST_SUITE_END()
