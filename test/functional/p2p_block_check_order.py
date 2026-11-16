#!/usr/bin/env python3
# Copyright (c) 2026-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that a block's own checks decide before its header is considered.

A block that fails CheckBlock is rejected for that reason, and its sender punished for it, whatever its
header says and whether or not the node wants the block:
1) A block with a timestamp too far in the future and a duplicated input is rejected for the duplicated
   input, not for the timestamp, and the peer is disconnected.
2) An unrequested block on a fork with less work, with a duplicated input, is checked all the same, and the
   peer is disconnected.

This is the order of a node that checks each block on the message handler: the test passes there too. Its
node is out of initial block download, where a block is checked on the message handler here as well: the
test holds the order against a change of it and does not reach the receive tasks of the connect pool.
"""
import time

from test_framework.blocktools import (
    MAX_FUTURE_BLOCK_TIME,
    create_block,
    create_tx_with_script,
)
from test_framework.messages import COIN
from test_framework.p2p import P2PDataStore
from test_framework.script import OP_TRUE
from test_framework.test_framework import BitcoinTestFramework


class BlockCheckOrderTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

    def run_test(self):
        node = self.nodes[0]
        peer = node.add_p2p_connection(P2PDataStore())

        # A spendable coinbase, matured.
        tip = node.getbestblockhash()
        best = node.getblock(tip)
        block1 = create_block(int(tip, 16), height=best["height"] + 1, ntime=best["time"] + 1)
        block1.solve()
        peer.send_blocks_and_test([block1], node, success=True)
        self.generatetoaddress(node, 100, node.get_deterministic_priv_key().address)

        def duplicated_input_tx():
            tx = create_tx_with_script(block1.vtx[0], 0, script_sig=bytes([OP_TRUE]), amount=50 * COIN)
            tx.vin.append(tx.vin[0])
            return tx

        self.log.info("A block with a future timestamp and a duplicated input is rejected for the input")
        tip = node.getbestblockhash()
        best = node.getblock(tip)
        t = int(time.time())
        node.setmocktime(t)
        block = create_block(int(tip, 16), height=best["height"] + 1, ntime=t + MAX_FUTURE_BLOCK_TIME + 1, txlist=[duplicated_input_tx()])
        block.solve()
        peer.send_blocks_and_test([block], node, force_send=True, success=False,
                                  reject_reason='bad-txns-inputs-duplicate', expect_disconnect=True)
        node.setmocktime(0)

        self.log.info("An unrequested block with less work and a duplicated input is checked and its sender punished")
        peer = node.add_p2p_connection(P2PDataStore())
        fork_parent = node.getblockhash(50)
        fork_header = node.getblockheader(fork_parent)
        block = create_block(int(fork_parent, 16), height=51, ntime=fork_header["time"] + 1, txlist=[duplicated_input_tx()])
        block.solve()
        peer.send_blocks_and_test([block], node, force_send=True, success=False,
                                  reject_reason='bad-txns-inputs-duplicate', expect_disconnect=True)


if __name__ == '__main__':
    BlockCheckOrderTest(__file__).main()
