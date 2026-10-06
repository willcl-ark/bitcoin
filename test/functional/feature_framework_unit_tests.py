#!/usr/bin/env python3
# Copyright (c) 2017-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Framework unit tests

Unit tests for the test framework.
"""

import sys
from time import perf_counter
import unittest

from test_framework.test_framework import TEST_EXIT_PASSED, TEST_EXIT_FAILED
from test_framework.test_profile import (
    profile_section,
    record_duration,
    set_metadata as set_profile_metadata,
    write_profile,
)

# List of framework modules containing unit tests. Should be kept in sync with
# the output of `git grep unittest.TestCase ./test/functional/test_framework`
TEST_FRAMEWORK_MODULES = [
    "address",
    "crypto.bip324_cipher",
    "blocktools",
    "compressor",
    "crypto.chacha20",
    "crypto.ellswift",
    "extendedkey",
    "key",
    "messages",
    "crypto.muhash",
    "crypto.poly1305",
    "crypto.ripemd160",
    "crypto.secp256k1",
    "crypto.siphash",
    "script",
    "script_util",
    "segwit_addr",
    "test_profile",
    "wallet_util",
]


class ProfiledTestResult(unittest.TextTestResult):
    def startTest(self, test):
        self._profile_start_time = perf_counter()
        super().startTest(test)

    def stopTest(self, test):
        record_duration(f"unit_test.{test.id()}", perf_counter() - self._profile_start_time)
        super().stopTest(test)


def run_unit_tests():
    set_profile_metadata(
        test_file=__file__,
        test_name="feature_framework_unit_tests.py",
        unit_test_modules=TEST_FRAMEWORK_MODULES,
    )
    test_framework_tests = unittest.TestSuite()
    for module in TEST_FRAMEWORK_MODULES:
        test_framework_tests.addTest(
            unittest.TestLoader().loadTestsFromName(f"test_framework.{module}")
        )
    with profile_section("framework_unit_tests.run"):
        result = unittest.TextTestRunner(
            stream=sys.stdout,
            verbosity=1,
            failfast=True,
            resultclass=ProfiledTestResult,
        ).run(test_framework_tests)
    exit_code = TEST_EXIT_PASSED if result.wasSuccessful() else TEST_EXIT_FAILED
    write_profile(
        status="passed" if result.wasSuccessful() else "failed",
        exit_code=exit_code,
    )
    sys.exit(exit_code)


if __name__ == "__main__":
    run_unit_tests()

