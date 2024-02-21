#include <policy/feerate.h>
#include <policy/mempool_fees.h>
#include <test/util/txmempool.h>
#include <timedata.h>
#include <txmempool.h>
#include <uint256.h>
#include <util/strencodings.h>
#include <util/time.h>
#include <validation.h>


#include <test/util/setup_common.h>

#include <memory>
#include <string>

#include <boost/test/unit_test.hpp>

const uint64_t SMALL_BYTES_DATA = 200;
const CAmount SMALL_BYTES_DATA_FEE = 11000;
const uint64_t LARGE_BYTES_DATA = 20200;
const CAmount LARGE_BYTES_DATA_FEE = 1100000;


BOOST_FIXTURE_TEST_SUITE(mempoolestimator_tests, TestChain100Setup)


BOOST_AUTO_TEST_CASE(MempoolEstimator)
{
    m_node.mempool.reset();
    m_node.mempool = std::make_unique<CTxMemPool>(MemPoolOptionsForTest(m_node));
    m_node.mempool_fee_estimator = std::make_unique<MemPoolPolicyEstimator>();
    std::string err_message;

    LOCK(cs_main);
    LOCK(m_node.mempool->cs);

    // Test case 1: confTarget > MAX_CONF_TARGET
    auto fee_estimate = m_node.mempool_fee_estimator->EstimateFeeWithMemPool(m_node.chainman->ActiveChainstate(), *(m_node.mempool), MAX_CONF_TARGET + 1, false, err_message);
    BOOST_CHECK(fee_estimate == CFeeRate(0));
    BOOST_CHECK(err_message == strprintf("Confirmation target %u is above maximum limit of %u, mempool conditions might change and estimates above %u are unreliable.\n", MAX_CONF_TARGET + 1, MAX_CONF_TARGET, MAX_CONF_TARGET));

    // Test case 2: Mempool not loaded
    fee_estimate = m_node.mempool_fee_estimator->EstimateFeeWithMemPool(m_node.chainman->ActiveChainstate(), *m_node.mempool, 1, false, err_message);
    BOOST_CHECK(err_message == std::string("Mempool not finished loading, can't get accurate fee rate estimate."));
    BOOST_CHECK(fee_estimate == CFeeRate(0));

    // // Test case 3: No transactions available in the mempool yet
    m_node.mempool->SetLoadTried(true);
    fee_estimate = m_node.mempool_fee_estimator->EstimateFeeWithMemPool(m_node.chainman->ActiveChainstate(), *m_node.mempool, 1, false, err_message);
    BOOST_CHECK(fee_estimate == CFeeRate(0));
    BOOST_CHECK(err_message == std::string("No transactions available in the mempool yet."));

    // TestMemPoolEntryHelper entry;

}

BOOST_AUTO_TEST_SUITE_END()
