#!/usr/bin/env python3
# Copyright (c) 2026 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test sender inputs and regtest transaction handoff over both transports."""

import subprocess

from test_framework.messages import COutPoint, CTransaction, CTxIn, CTxOut
from test_framework.socks5 import AddressType, Socks5Command, Socks5Configuration, Socks5Server
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, p2p_port
from test_framework.wallet import MiniWallet

ONION = "pg6mmjiyjmcrsslvykfwnntlaru7p5svn6y2ymmju6nubxndf4pscryd.onion"


class BroadcastToolTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

    def skip_test_if_missing_module(self):
        self.skip_if_no_bitcoin_broadcast()

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

        wallet = MiniWallet(self.nodes[0])
        self.generate(wallet, 101)
        conf = Socks5Configuration()
        conf.addr = ("127.0.0.1", 0)
        conf.auth = True
        conf.destinations_factory = lambda host, port, client: {
            "actual_to_addr": "127.0.0.1", "actual_to_port": p2p_port(0),
        }
        proxy = Socks5Server(conf)
        proxy.start()
        try:
            for v2 in [False, True]:
                self.log.info("Regtest mempool handoff with v2transport=%s", v2)
                self.restart_node(0, extra_args=[f"-v2transport={int(v2)}"])
                tx = wallet.create_self_transfer()
                assert tx["txid"] not in self.nodes[0].getrawmempool()
                result = subprocess.run(
                    self.bins.broadcast_argv() + ["-chain=regtest", f"-proxy=127.0.0.1:{conf.addr[1]}", "-timeout=10", ONION],
                    input=tx["hex"].encode(), capture_output=True, timeout=30,
                )
                assert_equal(result.returncode, 0)
                assert_equal(result.stdout, b"")
                assert_equal(result.stderr, b"")
                self.wait_until(lambda: tx["txid"] in self.nodes[0].getrawmempool())
                assert_equal(self.nodes[0].getrawtransaction(tx["txid"]), tx["hex"])
                # A v1-only node rejects the initial v2 attempt; fallback must
                # use another authenticated SOCKS connection to the same onion.
                for _ in range(1 if v2 else 2):
                    command = proxy.queue.get(timeout=10)
                    assert isinstance(command, Socks5Command)
                    assert_equal(command.atyp, AddressType.DOMAINNAME)
                    assert_equal(command.addr, ONION.encode())
                    assert_equal(command.port, 18444)
                    assert_equal(command.username, str(bytearray(b"<torS0X>0")))
                    assert command.password is not None
                assert proxy.queue.empty()
        finally:
            proxy.stop()
            proxy.s.close()


if __name__ == '__main__':
    BroadcastToolTest(__file__).main()
