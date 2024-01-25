// Copyright (c) 2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <tinyformat.h>
#include <util/result.h>

#include <boost/test/unit_test.hpp>


BOOST_AUTO_TEST_SUITE(result_tests)

struct NoCopy {
    NoCopy(int n) : m_n{std::make_unique<int>(n)} {}
    std::unique_ptr<int> m_n;
};

bool operator==(const NoCopy& a, const NoCopy& b)
{
    return *a.m_n == *b.m_n;
}

std::ostream& operator<<(std::ostream& os, const NoCopy& o)
{
    return os << "NoCopy(" << *o.m_n << ")";
}

util::Result<int> IntFn(int i, bool success)
{
    if (success) return i;
    return util::Error{strprintf("int %i error.", i)};
}

util::Result<std::string> StrFn(std::string s, bool success)
{
    if (success) return s;
    return util::Error{strprintf("str %s error.", s)};
}

util::Result<NoCopy> NoCopyFn(int i, bool success)
{
    if (success) return {i};
    return util::Error{strprintf("nocopy %i error.", i)};
}

template <typename T>
void ExpectResult(const util::Result<T>& result, bool success, const std::string& str)
{
    BOOST_CHECK_EQUAL(bool(result), success);
    BOOST_CHECK_EQUAL(util::ErrorString(result), str);
}

template <typename T, typename... Args>
void ExpectSuccess(const util::Result<T>& result, const std::string& str, Args&&... args)
{
    ExpectResult(result, true, str);
    BOOST_CHECK_EQUAL(result.has_value(), true);
    BOOST_CHECK_EQUAL(result.value(), T{std::forward<Args>(args)...});
    BOOST_CHECK_EQUAL(&result.value(), &*result);
}

template <typename T, typename... Args>
void ExpectFail(const util::Result<T>& result, const std::string& str)
{
    ExpectResult(result, false, str);
}

BOOST_AUTO_TEST_CASE(check_returned)
{
    ExpectSuccess(IntFn(5, true), {}, 5);
    ExpectFail(IntFn(5, false), "int 5 error.");
    ExpectSuccess(NoCopyFn(5, true), {}, 5);
    ExpectFail(NoCopyFn(5, false), "nocopy 5 error.");
    ExpectSuccess(StrFn("S", true), {}, "S");
    ExpectFail(StrFn("S", false), "str S error.");
}

BOOST_AUTO_TEST_CASE(check_value_or)
{
    BOOST_CHECK_EQUAL(IntFn(10, true).value_or(20), 10);
    BOOST_CHECK_EQUAL(IntFn(10, false).value_or(20), 20);
    BOOST_CHECK_EQUAL(NoCopyFn(10, true).value_or(20), 10);
    BOOST_CHECK_EQUAL(NoCopyFn(10, false).value_or(20), 20);
    BOOST_CHECK_EQUAL(StrFn("A", true).value_or("B"), "A");
    BOOST_CHECK_EQUAL(StrFn("A", false).value_or("B"), "B");
}

BOOST_AUTO_TEST_SUITE_END()
