#!/usr/bin/env python3
# Copyright (c) 2026 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test standalone transaction sender input and command handling."""

import subprocess

from test_framework.messages import COutPoint, CTransaction, CTxIn, CTxOut
from test_framework.socks5 import Socks5Configuration, Socks5Server
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

ONION = "pg6mmjiyjmcrsslvykfwnntlaru7p5svn6y2ymmju6nubxndf4pscryd.onion"


class BroadcastToolTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 0
        self.setup_clean_chain = True

    def skip_test_if_missing_module(self):
        self.skip_if_no_bitcoin_broadcast()

    def setup_network(self):
        pass

    def check_failure(self, args, tx_hex, reason):
        result = subprocess.run(self.bins.broadcast_argv() + args, input=tx_hex, capture_output=True, timeout=10)
        assert_equal(result.returncode, 1)
        assert_equal(result.stdout, b"")
        assert_equal(result.stderr, reason.encode() + b"\n")

    def run_test(self):
        self.bins = self.get_binaries()
        tx = CTransaction()
        tx.vin = [CTxIn(COutPoint(1, 0))]
        tx.vout = [CTxOut(1, b"")]
        tx_hex = tx.serialize().hex().encode()
        self.log.info("Help and version do not read stdin")
        for option in ["-help", "-version"]:
            with subprocess.Popen(self.bins.broadcast_argv() + [option], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE) as process:
                # Keep stdin open. Completion must not depend on its EOF.
                process.wait(timeout=10)
                assert_equal(process.returncode, 0)
                assert b"bitcoin-broadcast" in process.stdout.read()
                assert_equal(process.stderr.read(), b"")

        conf = Socks5Configuration()
        conf.addr = ("127.0.0.1", 0)
        conf.auth = True
        proxy = Socks5Server(conf)
        try:
            prefix = [f"-proxy=127.0.0.1:{conf.addr[1]}"]
            self.log.info("All options and destinations fail before connecting")
            for args, reason in [
                (["-unknown=" + ONION, ONION], "Invalid options."),
                (["-proxy", ONION], "Invalid options."),
                (["-noproxy", ONION], "Invalid options."),
                (["-chain=invalid", ONION], "Invalid chain."),
                (["-timeout=0", ONION], "Invalid timeout."),
                (["-timeout=18446744073709551615", ONION], "Invalid timeout."),
                ([], "Invalid onion destinations."),
                ([ONION, "invalid.onion"], "Invalid onion destinations."),
                ([ONION + "=ignored"], "Invalid onion destinations."),
                (["127.0.0.1"], "Invalid onion destinations."),
                (["[::1]"], "Invalid onion destinations."),
                (["-proxy=192.168.1.1:9050", ONION], "Invalid proxy endpoint."),
                (["-proxy=localhost:9050", ONION], "Invalid proxy endpoint."),
                (["-proxy=0.0.0.0:9050", ONION], "Invalid proxy endpoint."),
            ]:
                self.check_failure(prefix + args, tx_hex, reason)

            self.log.info("Malformed transaction records fail before connecting")
            for bad in [b"", b"0", b"zz", tx_hex + b"00", tx_hex + tx_hex, tx_hex + b"\r", tx_hex + b"\n\n",
                        b" " + tx_hex, tx_hex + b" \n", tx_hex + b"\n00", b"0" * 8_000_003]:
                self.check_failure(prefix + [ONION], bad, "Invalid transaction input.")
            tx.vin = [CTxIn(COutPoint(0, 0xffffffff), b"\x00\x00")]
            self.check_failure(prefix + [ONION], tx.serialize().hex().encode(), "Invalid transaction input.")

            # Leave the listener unserved so every connection remains observable.
            proxy.s.setblocking(False)
            try:
                connection, _ = proxy.s.accept()
                connection.close()
                raise AssertionError("Invalid input contacted proxy")
            except BlockingIOError:
                pass
        finally:
            if proxy.is_running():
                proxy.stop()
            proxy.s.close()


if __name__ == '__main__':
    BroadcastToolTest(__file__).main()
