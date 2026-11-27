#!/usr/bin/env python3
# Copyright (c) 2026-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that a block announced via NewPoWValidBlock is served only when valid.

A valid block at the tip is stored off cs_main, announced via NewPoWValidBlock,
connected, and served to peers requesting it. A block that passes header/PoW
checks and fails connection (bad-cb-amount) must disconnect its sender and must
not be served to subsequent peers.
"""

from test_framework.blocktools import (
    create_block,
    create_coinbase,
)
from test_framework.messages import (
    CInv,
    MSG_BLOCK,
    MSG_WITNESS_FLAG,
    msg_block,
    msg_getdata,
)
from test_framework.p2p import (
    P2PInterface,
    p2p_lock,
)
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal


class FailedBlockNotServedTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1

    def next_block(self, coinbase_value=50):
        node = self.nodes[0]
        tip = node.getbestblockhash()
        height = node.getblockcount() + 1
        block = create_block(
            int(tip, 16),
            create_coinbase(height, nValue=coinbase_value),
            ntime=node.getblock(tip)["time"] + 1,
        )
        block.solve()
        return block

    def request_block(self, block):
        node = self.nodes[0]
        requester = node.add_p2p_connection(P2PInterface())
        requester.send_and_ping(msg_getdata([CInv(MSG_BLOCK | MSG_WITNESS_FLAG, block.hash_int)]))
        with p2p_lock:
            served = (
                "block" in requester.last_message
                and requester.last_message["block"].block.hash_int == block.hash_int
            )
        requester.peer_disconnect()
        requester.wait_for_disconnect()
        return served

    def run_test(self):
        node = self.nodes[0]
        self.generate(node, 1)
        assert not node.getblockchaininfo()["initialblockdownload"]

        self.log.info("A valid tip block is announced, connected, and served")
        sender = node.add_p2p_connection(P2PInterface())
        valid = self.next_block()
        with node.assert_debug_log(expected_msgs=[f"NewPoWValidBlock: block hash={valid.hash_hex}"]):
            sender.send_and_ping(msg_block(valid))
        assert_equal(node.getbestblockhash(), valid.hash_hex)
        assert self.request_block(valid)

        self.log.info("A tip block that fails connection disconnects its sender and is not served")
        tip = node.getbestblockhash()
        invalid = self.next_block(coinbase_value=51)
        with node.assert_debug_log(expected_msgs=[f"NewPoWValidBlock: block hash={invalid.hash_hex}", "bad-cb-amount"]):
            sender.send_without_ping(msg_block(invalid))
            sender.wait_for_disconnect()
        assert_equal(node.getbestblockhash(), tip)
        assert not self.request_block(invalid)


if __name__ == "__main__":
    FailedBlockNotServedTest(__file__).main()
