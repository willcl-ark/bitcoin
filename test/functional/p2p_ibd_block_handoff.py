#!/usr/bin/env python3
# Copyright (c) 2026-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test IBD-only P2P block handoff, ibdconnect thread, deferred PONGs, and shutdown.

During IBD, ProcessBlock() stores received blocks off cs_main, places them in
BlockHandoffCache, and hands activation to the lazy ibdconnect thread while
deferring PONG responses (bounded per peer) until earlier handed-off blocks
have finished connecting. Verifies:
1. Blocks connect on the [ibdconnect] thread and PONG is causally ordered after them.
2. Sending more than 16 PINGs behind handed-off blocks stays bounded and completes cleanly.
3. Out-of-order block delivery connects cleanly via BlockHandoffCache once the parent arrives.
4. Mutated and invalid blocks handed off during IBD disconnect their sender without corrupting tip.
5. Clean node shutdown while ibdconnect is active terminates promptly without hanging.
"""

import copy
import time

from test_framework.blocktools import (
    TIME_GENESIS_BLOCK,
    create_block,
    create_coinbase,
)
from test_framework.messages import (
    CBlockHeader,
    msg_block,
    msg_headers,
    msg_ping,
)
from test_framework.p2p import (
    P2PDataStore,
    P2PInterface,
    p2p_lock,
)
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)

BATCH = 12


class IBDBlockHandoffTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.extra_args = [["-logthreadnames=1", "-par=4", "-prevoutfetchthreads=4"]]

    def build_blocks(self, prev_hash, height, count, *, first_coinbase_value=50, time_offset=0):
        blocks = []
        for i in range(count):
            cb = create_coinbase(height + i, nValue=first_coinbase_value if i == 0 else 50)
            blk = create_block(prev_hash, cb, ntime=TIME_GENESIS_BLOCK + height + i + time_offset)
            blk.solve()
            blocks.append(blk)
            prev_hash = blk.hash_int
        return blocks

    def sync_from_new_peer(self, blocks):
        node = self.nodes[0]
        peer = node.add_p2p_connection(P2PDataStore())
        with p2p_lock:
            for blk in blocks:
                peer.block_store[blk.hash_int] = blk
            peer.last_block_hash = blocks[-1].hash_int
        peer.send_without_ping(msg_headers([CBlockHeader(b) for b in blocks]))
        self.wait_until(lambda: node.getbestblockhash() == blocks[-1].hash_hex, timeout=60)
        peer.peer_disconnect()
        peer.wait_for_disconnect()

    def run_test(self):
        node = self.nodes[0]
        node.setmocktime(int(time.time()))

        self.log.info("Deliver a batch of IBD blocks and verify [ibdconnect] + causal deferred PONG")
        peer = node.add_p2p_connection(P2PInterface())
        tip_hex = node.getbestblockhash()
        height = 1
        blocks = self.build_blocks(int(tip_hex, 16), height, BATCH)
        log_pos = node.debug_log_size()
        peer.send_without_ping(msg_headers([CBlockHeader(b) for b in blocks]))
        for blk in blocks:
            peer.send_without_ping(msg_block(blk))
        # Flood 20 pings (> MAX_DEFERRED_PONGS_PER_PEER = 16) with nonces disjoint
        # from sync_with_ping()'s counter, followed by sync_with_ping().
        for nonce in range(1000, 1020):
            peer.send_without_ping(msg_ping(nonce=nonce))
        peer.sync_with_ping()
        # No wait_until needed: deferred PONG guarantees ibdconnect already connected the full batch.
        assert_equal(node.getblockcount(), BATCH)
        assert_equal(node.getbestblockhash(), blocks[-1].hash_hex)
        assert node.getblockchaininfo()["initialblockdownload"]

        with open(node.debug_log_path, encoding="utf-8") as log:
            log.seek(log_pos)
            text = log.read()
        assert "[ibdconnect]" in text and f"UpdateTip: new best={blocks[-1].hash_hex}" in text

        tip_hex = blocks[-1].hash_hex
        height += BATCH

        self.log.info("Out-of-order IBD delivery stores blocks in handoff cache and connects once parent arrives")
        ooo = self.build_blocks(int(tip_hex, 16), height, 3)
        peer.send_and_ping(msg_headers([CBlockHeader(b) for b in ooo]))
        peer.send_and_ping(msg_block(ooo[2]))
        peer.send_and_ping(msg_block(ooo[1]))
        assert_equal(node.getbestblockhash(), tip_hex)
        peer.send_and_ping(msg_block(ooo[0]))
        self.wait_until(lambda: node.getbestblockhash() == ooo[2].hash_hex)
        assert_equal(node.getblockcount(), height + 2)
        assert_equal(node.getbestblockhash(), ooo[2].hash_hex)
        tip_hex = ooo[2].hash_hex
        height += 3

        self.log.info("Mutated block during IBD disconnects peer and is not stored")
        blocks = self.build_blocks(int(tip_hex, 16), height, 4)
        mutated = copy.deepcopy(blocks[0])
        mutated.vtx[0].vout[0].nValue -= 1
        assert_equal(mutated.hash_int, blocks[0].hash_int)
        peer.send_without_ping(msg_headers([CBlockHeader(b) for b in blocks]))
        with node.assert_debug_log(expected_msgs=["mutated block"]):
            peer.send_without_ping(msg_block(mutated))
            peer.wait_for_disconnect()
        assert_equal(node.getbestblockhash(), tip_hex)
        assert_raises_rpc_error(-1, "Block not available (not fully downloaded)", node.getblock, blocks[0].hash_hex)

        self.log.info("Another peer delivers the valid batch cleanly")
        self.sync_from_new_peer(blocks)
        tip_hex = blocks[-1].hash_hex
        height += 4

        self.log.info("Invalid block connected by ibdconnect disconnects its sender")
        peer = node.add_p2p_connection(P2PInterface())
        invalid = self.build_blocks(int(tip_hex, 16), height, 3, first_coinbase_value=51)
        peer.send_without_ping(msg_headers([CBlockHeader(b) for b in invalid]))
        with node.assert_debug_log(expected_msgs=["bad-cb-amount"]):
            for blk in invalid:
                peer.send_without_ping(msg_block(blk))
            peer.wait_for_disconnect()
        assert_equal(node.getbestblockhash(), tip_hex)

        self.log.info("Valid chain is accepted from a new peer and shutdown while ibdconnect is active succeeds")
        valid_blocks = self.build_blocks(int(tip_hex, 16), height, 4, time_offset=1)
        self.sync_from_new_peer(valid_blocks)
        assert node.getblockchaininfo()["initialblockdownload"]
        # Clean shutdown while ibdconnect is still active must not hang.
        self.stop_node(0)
        self.start_node(0)
        assert_equal(node.getbestblockhash(), valid_blocks[-1].hash_hex)


if __name__ == "__main__":
    IBDBlockHandoffTest(__file__).main()
