#!/usr/bin/env python3
# Copyright (c) 2026-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test bounded P2P block admission during deep IBD."""

from test_framework.blocktools import create_block, create_coinbase
from test_framework.messages import CBlockHeader
from test_framework.messages import MAX_HEADERS_RESULTS
from test_framework.messages import msg_block
from test_framework.messages import msg_headers
from test_framework.messages import msg_ping
from test_framework.p2p import P2PDataStore
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal


class IbdBatchTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        common_args = ["-assumevalid=0", "-par=2", "-prevoutfetchthreads=2", "-dbcache=64", "-debug=bench"]
        self.extra_args = [common_args + ["-ibdblockcache=1"], common_args + ["-ibdblockcache=0"]]

    def setup_network(self):
        self.setup_nodes()

    def send_blocks_and_ping(self, peer, blocks, nonce):
        messages = [msg_block(block) for block in blocks]
        messages.append(msg_ping(nonce=nonce))
        raw_messages = b"".join(peer.build_message(message) for message in messages)
        peer.send_raw_message(raw_messages)
        peer.wait_until(
            lambda: peer.last_message.get("pong") is not None and
            peer.last_message["pong"].nonce == nonce
        )

    def run_test(self):
        node, no_cache_node = self.nodes
        peer = node.add_p2p_connection(P2PDataStore(), supports_v2_p2p=False)
        no_cache_peer = no_cache_node.add_p2p_connection(P2PDataStore(), supports_v2_p2p=False)

        genesis = node.getblock(node.getbestblockhash())
        previous_hash = int(genesis["hash"], 16)
        previous_time = genesis["time"]
        first_block = create_block(previous_hash, height=1, ntime=previous_time + 1)
        first_block.solve()
        peer.send_blocks_and_test([first_block], node)
        no_cache_peer.send_blocks_and_test([first_block], no_cache_node)
        assert_equal(node.getblockchaininfo()["initialblockdownload"], True)
        assert_equal(no_cache_node.getblockchaininfo()["initialblockdownload"], True)
        previous_hash = first_block.hash_int
        previous_time = first_block.nTime

        # More than 14 days of regtest proof work keeps the active tip eligible
        # for the bounded deep-IBD admission path while the body burst arrives.
        blocks = []
        for height in range(2, 2051):
            previous_time += 1
            coinbase_value = 51 if height == 10 else 50
            block = create_block(
                previous_hash,
                create_coinbase(height, nValue=coinbase_value),
                height=height,
                ntime=previous_time,
            )
            block.solve()
            blocks.append(block)
            previous_hash = block.hash_int

        headers = [CBlockHeader(block) for block in blocks]
        for offset in range(0, len(headers), MAX_HEADERS_RESULTS):
            peer.send_and_ping(msg_headers(headers[offset:offset + MAX_HEADERS_RESULTS]))
            no_cache_peer.send_and_ping(msg_headers(headers[offset:offset + MAX_HEADERS_RESULTS]))
        assert_equal(node.getblockheader(headers[-1].hash_hex)["height"], 2050)
        assert_equal(no_cache_node.getblockheader(headers[-1].hash_hex)["height"], 2050)
        assert_equal(node.getblockchaininfo()["initialblockdownload"], True)

        self.log.info("Queue linked block bodies followed by a ping barrier")
        self.send_blocks_and_ping(peer, blocks[:4], nonce=1)
        assert_equal(node.getbestblockhash(), blocks[3].hash_hex)

        self.log.info("Retain out-of-order bodies across a message barrier")
        self.send_blocks_and_ping(peer, [blocks[6]], nonce=2)
        assert_equal(node.getbestblockhash(), blocks[3].hash_hex)
        self.send_blocks_and_ping(peer, [blocks[5]], nonce=3)
        assert_equal(node.getbestblockhash(), blocks[3].hash_hex)
        with node.assert_debug_log(expected_msgs=[
            "IBD batch: blocks=2 first=6 last=7",
            "retained_hits=2 disk_reads=0",
            "Block body load: height=8 retained_hits=0 disk_reads=1",
        ], timeout=10):
            self.send_blocks_and_ping(peer, [blocks[4]], nonce=4)
        assert_equal(node.getbestblockhash(), blocks[6].hash_hex)

        self.log.info("Check that zero disables cross-barrier body retention")
        self.send_blocks_and_ping(no_cache_peer, blocks[:4], nonce=10)
        assert_equal(no_cache_node.getbestblockhash(), blocks[3].hash_hex)
        self.send_blocks_and_ping(no_cache_peer, [blocks[6]], nonce=11)
        assert_equal(no_cache_node.getbestblockhash(), blocks[3].hash_hex)
        self.send_blocks_and_ping(no_cache_peer, [blocks[5]], nonce=12)
        assert_equal(no_cache_node.getbestblockhash(), blocks[3].hash_hex)
        with no_cache_node.assert_debug_log(expected_msgs=[
            "Block body load: height=7 retained_hits=0 disk_reads=1",
            "Block body load: height=8 retained_hits=0 disk_reads=1",
        ], timeout=10):
            self.send_blocks_and_ping(no_cache_peer, [blocks[4]], nonce=13)
        assert_equal(no_cache_node.getbestblockhash(), blocks[6].hash_hex)

        self.log.info("Check invalid-body attribution and valid-prefix preservation")
        messages = [msg_block(block) for block in blocks[7:10]]
        messages.insert(2, msg_block(blocks[8]))
        raw_messages = b"".join(peer.build_message(message) for message in messages)
        peer.send_raw_message(raw_messages)
        peer.wait_for_disconnect()
        assert_equal(node.getbestblockhash(), blocks[7].hash_hex)


if __name__ == "__main__":
    IbdBatchTest(__file__).main()
