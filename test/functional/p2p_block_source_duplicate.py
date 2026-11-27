#!/usr/bin/env python3
# Copyright (c) 2026-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that a duplicate delivery of a stored block does not clear its original sender.

Peer A sends an invalid child block (missing input) before its parent so the child
is stored without connecting yet. Either Peer B or Peer A itself sends a duplicate
copy of the same child. Peer A then sends the parent: the child connects and fails,
and Peer A (not Peer B) must be disconnected. Tested both during IBD (async handoff)
and post-IBD.
"""

import time

from test_framework.blocktools import (
    TIME_GENESIS_BLOCK,
    create_block,
    create_coinbase,
)
from test_framework.messages import (
    CBlockHeader,
    COutPoint,
    CTransaction,
    CTxIn,
    CTxOut,
    msg_block,
    msg_headers,
)
from test_framework.p2p import P2PInterface
from test_framework.script import CScript, OP_TRUE
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal


class BlockSourceDuplicateTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.extra_args = [["-par=4", "-prevoutfetchthreads=4"]]

    def run_duplicate_scenario(self, node, *, block_time, same_peer=False):
        tip = node.getbestblockhash()
        height = node.getblockcount() + 1

        parent = create_block(int(tip, 16), create_coinbase(height), ntime=block_time)
        parent.solve()
        bad = CTransaction()
        bad.vin = [CTxIn(COutPoint(0x1234 + height, 0), CScript([OP_TRUE]))]
        bad.vout = [CTxOut(1000, CScript([OP_TRUE]))]
        child = create_block(parent.hash_int, create_coinbase(height + 1), ntime=block_time + 1, txlist=[bad])
        child.solve()

        peer_a = node.add_p2p_connection(P2PInterface())
        peer_b = None if same_peer else node.add_p2p_connection(P2PInterface())

        peer_a.send_and_ping(msg_headers([CBlockHeader(parent), CBlockHeader(child)]))
        peer_a.send_and_ping(msg_block(child))
        assert_equal(node.getblockheader(child.hash_hex)["confirmations"], -1)

        # Duplicate from either Peer B or Peer A itself must not erase Peer A's mapBlockSource ownership.
        if same_peer:
            peer_a.send_and_ping(msg_block(child))
        else:
            peer_b.send_and_ping(msg_block(child))

        with node.assert_debug_log(expected_msgs=["bad-txns-inputs-missingorspent"]):
            peer_a.send_without_ping(msg_block(parent))
            peer_a.wait_for_disconnect()

        assert_equal(node.getbestblockhash(), parent.hash_hex)
        if peer_b is not None:
            assert peer_b.is_connected
        node.disconnect_p2ps()

    def run_test(self):
        node = self.nodes[0]
        now = int(time.time())
        node.setmocktime(now)

        self.log.info("Test cross-peer duplicate block source preservation during IBD (async ibdconnect handoff)")
        assert node.getblockchaininfo()["initialblockdownload"]
        self.run_duplicate_scenario(node, block_time=TIME_GENESIS_BLOCK + 10, same_peer=False)
        assert node.getblockchaininfo()["initialblockdownload"]

        self.log.info("Test same-peer duplicate block source preservation during IBD (async ibdconnect handoff)")
        self.run_duplicate_scenario(node, block_time=TIME_GENESIS_BLOCK + 20, same_peer=True)
        assert node.getblockchaininfo()["initialblockdownload"]

        self.log.info("Exit IBD and test cross-peer and same-peer duplicate block source preservation at chain tip")
        self.generate(node, 1)
        assert not node.getblockchaininfo()["initialblockdownload"]
        best_time = node.getblock(node.getbestblockhash())["time"]
        self.run_duplicate_scenario(node, block_time=best_time + 1, same_peer=False)
        best_time = node.getblock(node.getbestblockhash())["time"]
        self.run_duplicate_scenario(node, block_time=best_time + 1, same_peer=True)


if __name__ == "__main__":
    BlockSourceDuplicateTest(__file__).main()
