#!/usr/bin/env python3
# Copyright (c) 2026 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Check that getopenrpcinfo RPC is callable and serializable as valid json."""

import json

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal


class OpenRPCDocTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1

    def run_test(self):
        self.log.info("Calling getopenrpcinfo")
        openrpc = self.nodes[0].getopenrpcinfo()
        json.dumps(openrpc)

        getblock_method = next(
            method for method in openrpc["methods"] if method["name"] == "getblock"
        )
        verbosity_2 = next(
            schema
            for schema in getblock_method["result"]["schema"]["oneOf"]
            if schema["description"] == "for verbosity = 2"
        )
        assert_equal(verbosity_2["type"], "object")
        assert_equal(verbosity_2["additionalProperties"], True)


if __name__ == "__main__":
    OpenRPCDocTest(__file__).main()
