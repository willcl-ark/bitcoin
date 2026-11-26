// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/message_tip_rule.h>

#include <boost/test/unit_test.hpp>

#include <set>
#include <string_view>

BOOST_AUTO_TEST_SUITE(message_tip_rule_tests)

BOOST_AUTO_TEST_CASE(every_message_type_has_unique_rule)
{
    std::set<std::string_view> seen;
    for (const node::MessageTipRule& row : node::MESSAGE_TIP_RULES) {
        BOOST_CHECK(seen.insert(row.type).second);
    }
    for (const std::string& type : ALL_NET_MESSAGE_TYPES) {
        BOOST_CHECK(node::TipRuleOf(type).has_value());
        BOOST_CHECK(seen.contains(type));
    }
    BOOST_CHECK(!node::TipRuleOf("not_a_real_message_type").has_value());
}

BOOST_AUTO_TEST_SUITE_END()
