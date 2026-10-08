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
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [["-assumevalid=0", "-par=2", "-prevoutfetchthreads=2"]]

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
        node = self.nodes[0]
        peer = node.add_p2p_connection(P2PDataStore(), supports_v2_p2p=False)

        genesis = node.getblock(node.getbestblockhash())
        previous_hash = int(genesis["hash"], 16)
        previous_time = genesis["time"]
        first_block = create_block(previous_hash, height=1, ntime=previous_time + 1)
        first_block.solve()
        peer.send_blocks_and_test([first_block], node)
        assert_equal(node.getblockchaininfo()["initialblockdownload"], True)
        previous_hash = first_block.hash_int
        previous_time = first_block.nTime

        # More than 14 days of regtest proof work keeps the active tip eligible
        # for the bounded deep-IBD admission path while the body burst arrives.
        blocks = []
        for height in range(2, 2051):
            previous_time += 1
            coinbase_value = 51 if height == 7 else 50
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
        assert_equal(node.getblockheader(headers[-1].hash_hex)["height"], 2050)
        assert_equal(node.getblockchaininfo()["initialblockdownload"], True)

        self.log.info("Queue linked block bodies followed by a ping barrier")
        self.send_blocks_and_ping(peer, blocks[:4], nonce=1)
        assert_equal(node.getbestblockhash(), blocks[3].hash_hex)

        self.log.info("Check invalid-body attribution and valid-prefix preservation")
        messages = [msg_block(block) for block in blocks[4:7]]
        messages.insert(2, msg_block(blocks[5]))
        raw_messages = b"".join(peer.build_message(message) for message in messages)
        peer.send_raw_message(raw_messages)
        peer.wait_for_disconnect()
        assert_equal(node.getbestblockhash(), blocks[4].hash_hex)


if __name__ == "__main__":
    IbdBatchTest(__file__).main()
