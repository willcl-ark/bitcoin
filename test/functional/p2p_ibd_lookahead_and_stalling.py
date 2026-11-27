#!/usr/bin/env python3
# Copyright (c) 2026-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test IBD download lookahead bounds and stalling detection under parallel IBD settings.

Verifies two properties of FindNextBlocksToDownload() / FindNextBlocks():
1. During IBD, download lookahead is bounded by ActiveHeight() + BLOCK_DOWNLOAD_WINDOW (1024):
   blocks beyond height 1024 are not requested while ActiveHeight() remains at 0, and once
   block 1 arrives and ActiveHeight() advances, the remaining blocks are requested and synced.
2. Standard IBD stalling detection (2s -> 4s -> 8s exponential backoff) continues to
   disconnect true stallers with parallel IBD worker settings enabled (-par=4 -prevoutfetchthreads=4).
"""

import time

from test_framework.blocktools import create_block
from test_framework.messages import (
    CBlockHeader,
    msg_block,
    msg_headers,
)
from test_framework.p2p import p2p_lock
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

from p2p_ibd_stalling import P2PStaller

BLOCK_DOWNLOAD_WINDOW = 1024


class P2PIBDLookaheadAndStallingTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 2
        self.extra_args = [
            ["-par=4", "-prevoutfetchthreads=4"],
            ["-par=4", "-prevoutfetchthreads=4", "-prune=550"],
        ]

    def setup_network(self):
        self.setup_nodes()

    def build_chain(self, node, count):
        tip = int(node.getbestblockhash(), 16)
        block_time = node.getblock(node.getbestblockhash())["time"] + 1
        blocks = []
        block_dict = {}
        for height in range(1, count + 1):
            blk = create_block(tip, height=height, ntime=block_time)
            blk.solve()
            blocks.append(blk)
            block_dict[blk.hash_int] = blk
            tip = blk.hash_int
            block_time += 1
        return blocks, block_dict

    def test_lookahead_bound_and_recovery(self, node_idx, blocks, block_dict):
        node = self.nodes[node_idx]
        # Keep timestamps in the past so the node stays in IBD.
        mocktime = int(time.time()) + 10
        node.setmocktime(mocktime)

        # Withhold block 1 so ActiveHeight() stays 0 while blocks 2..1024 are downloaded and stored.
        withheld = blocks[0]
        peer = node.add_outbound_p2p_connection(
            P2PStaller([withheld.hash_int]),
            p2p_idx=0,
            connection_type="outbound-full-relay",
        )
        peer.block_store = block_dict
        peer.send_and_ping(msg_headers([CBlockHeader(b) for b in blocks]))

        # Wait until blocks 2..1024 have been delivered and only withheld block 1 remains in flight.
        self.wait_until(
            lambda: sum(len(p["inflight"]) for p in node.getpeerinfo()) == 1,
            timeout=120,
        )
        assert_equal(node.getblockcount(), 0)

        # Blocks beyond ActiveHeight() + BLOCK_DOWNLOAD_WINDOW (1024) must NOT have been requested.
        with p2p_lock:
            requested_set = set(peer.getdata_requests)
        assert withheld.hash_int in requested_set
        assert blocks[BLOCK_DOWNLOAD_WINDOW - 1].hash_int in requested_set
        for beyond in blocks[BLOCK_DOWNLOAD_WINDOW:]:
            assert beyond.hash_int not in requested_set, f"Block beyond download window was requested prematurely"

        # Now deliver block 1: ActiveHeight() advances past 1024, unlocking requests for blocks 1025..1030.
        peer.send_without_ping(msg_block(withheld))
        self.wait_until(lambda: node.getblockcount() == len(blocks), timeout=120)
        assert_equal(node.getbestblockhash(), blocks[-1].hash_hex)
        node.disconnect_p2ps()

    def sync_all_peers(self, peers):
        for p in peers:
            if p.is_connected:
                p.sync_with_ping()

    def run_test(self):
        blocks, block_dict = self.build_chain(self.nodes[0], BLOCK_DOWNLOAD_WINDOW + 6)

        self.log.info("Verify true staller is disconnected when 1025-block window is blocked by peer 0")
        node0 = self.nodes[0]
        mocktime = int(time.time()) + 1
        node0.setmocktime(mocktime)
        stall_blocks = [blocks[0].hash_int, blocks[500].hash_int]
        peers = []
        headers_1024 = msg_headers([CBlockHeader(b) for b in blocks[:BLOCK_DOWNLOAD_WINDOW]])
        for i in range(3):
            p = node0.add_outbound_p2p_connection(
                P2PStaller(stall_blocks),
                p2p_idx=i,
                connection_type="outbound-full-relay",
            )
            p.block_store = block_dict
            p.send_and_ping(headers_1024)
            peers.append(p)
        self.wait_until(lambda: sum(len(p["inflight"]) for p in node0.getpeerinfo()) == len(stall_blocks), timeout=120)
        self.sync_all_peers(peers)

        # Pushing headers to 1025 triggers stall timer on peer 0; advancing mocktime by 3s disconnects it.
        headers_1025 = msg_headers([CBlockHeader(b) for b in blocks[:BLOCK_DOWNLOAD_WINDOW + 1]])
        with node0.assert_debug_log(expected_msgs=["Stall started"]):
            for p in peers:
                p.send_without_ping(headers_1025)
            self.sync_all_peers(peers)
        mocktime += 3
        node0.setmocktime(mocktime)
        peers[0].wait_for_disconnect()
        assert_equal(node0.num_test_p2p_connections(), 2)
        node0.disconnect_p2ps()

        self.log.info("Verify download lookahead bound (1024) and recovery in IBD + prune mode")
        self.test_lookahead_bound_and_recovery(1, blocks, block_dict)


if __name__ == "__main__":
    P2PIBDLookaheadAndStallingTest(__file__).main()
