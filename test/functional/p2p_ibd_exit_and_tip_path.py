#!/usr/bin/env python3
# Copyright (c) 2026-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test IBD exit teardown of ibdconnect and synchronous tip block + child tx handling.

During IBD, P2P blocks connect asynchronously on [ibdconnect]. Once a current-time
block arrives and the node exits IBD:
1. The [ibdconnect] thread exits via MaybeTeardownIBDResources().
2. Subsequent tip blocks connect synchronously on [msghand] (not [ibdconnect]).
3. A child transaction sent immediately behind a tip block that creates its input
   enters the mempool directly without passing through the orphanage.
"""

import time

from test_framework.blocktools import (
    TIME_GENESIS_BLOCK,
    add_witness_commitment,
    create_block,
    create_coinbase,
)
from test_framework.messages import (
    CBlockHeader,
    msg_block,
    msg_headers,
    msg_tx,
)
from test_framework.p2p import P2PInterface
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal
from test_framework.wallet import MiniWallet


class IBDExitAndTipPathTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.extra_args = [["-logthreadnames=1", "-debug=txpackages", "-par=4", "-prevoutfetchthreads=4"]]

    def run_test(self):
        node = self.nodes[0]
        now = int(time.time())
        node.setmocktime(now)
        peer = node.add_p2p_connection(P2PInterface())

        self.log.info("Connect old blocks during IBD via [ibdconnect]")
        prev_hash = int(node.getbestblockhash(), 16)
        ibd_blocks = []
        for h in range(1, 5):
            blk = create_block(prev_hash, create_coinbase(h), ntime=TIME_GENESIS_BLOCK + h)
            blk.solve()
            ibd_blocks.append(blk)
            prev_hash = blk.hash_int

        log_pos = node.debug_log_size()
        peer.send_without_ping(msg_headers([CBlockHeader(b) for b in ibd_blocks]))
        for blk in ibd_blocks:
            peer.send_without_ping(msg_block(blk))
        peer.sync_with_ping()
        assert_equal(node.getbestblockhash(), ibd_blocks[-1].hash_hex)
        assert node.getblockchaininfo()["initialblockdownload"]

        with open(node.debug_log_path, encoding="utf-8") as log:
            log.seek(log_pos)
            assert "[ibdconnect]" in log.read()

        self.log.info("Deliver a multi-block batch crossing max_tip_age to exit IBD and tear down [ibdconnect]")
        # Stage blocks 6..9 before block 5 so all 5 blocks are stored on disk when ibdconnect
        # enters ActivateBestChainParallelIBD: blocks 5..6 remain in IBD, block 7 crosses
        # max_tip_age (exiting IBD), and blocks 8..9 advance the post-IBD tip in the same run.
        exit_batch = []
        exit_times = [
            TIME_GENESIS_BLOCK + 5,
            TIME_GENESIS_BLOCK + 6,
            now,
            now + 1,
            now + 2,
        ]
        for offset, ntime in enumerate(exit_times):
            blk = create_block(prev_hash, create_coinbase(5 + offset), ntime=ntime)
            blk.solve()
            exit_batch.append(blk)
            prev_hash = blk.hash_int

        peer.send_and_ping(msg_headers([CBlockHeader(b) for b in exit_batch]))
        for blk in exit_batch[1:]:
            peer.send_without_ping(msg_block(blk))
        peer.sync_with_ping()
        assert_equal(node.getblockcount(), 4)
        assert node.getblockchaininfo()["initialblockdownload"]

        log_pos = node.debug_log_size()
        peer.send_and_ping(msg_block(exit_batch[0]))
        self.wait_until(lambda: node.getbestblockhash() == exit_batch[-1].hash_hex)
        assert_equal(node.getblockcount(), 4 + len(exit_batch))
        assert not node.getblockchaininfo()["initialblockdownload"]

        def log_from_pos():
            with open(node.debug_log_path, encoding="utf-8") as log:
                log.seek(log_pos)
                return log.read()

        self.wait_until(lambda: "ibdconnect thread exit" in log_from_pos())
        exit_log = log_from_pos()
        for blk in exit_batch:
            assert f"UpdateTip: new best={blk.hash_hex}" in exit_log, f"Missing UpdateTip for {blk.hash_hex}"

        self.log.info("Post-IBD tip blocks connect on [msghand] and immediate child txs enter mempool directly")
        wallet = MiniWallet(node)
        self.generate(wallet, 105)

        for _ in range(5):
            parent = wallet.create_self_transfer()
            child = wallet.create_self_transfer(utxo_to_spend=parent["new_utxo"])
            tip = node.getbestblockhash()
            block = create_block(
                int(tip, 16),
                create_coinbase(node.getblockcount() + 1),
                ntime=node.getblock(tip)["time"] + 1,
                txlist=[parent["tx"]],
            )
            add_witness_commitment(block)
            block.solve()
            assert parent["txid"] not in node.getrawmempool()

            log_pos = node.debug_log_size()
            with node.assert_debug_log(expected_msgs=[], unexpected_msgs=["stored orphan tx"]):
                peer.send_without_ping(msg_block(block))
                peer.send_and_ping(msg_tx(child["tx"]))
            assert_equal(node.getbestblockhash(), block.hash_hex)
            assert child["txid"] in node.getrawmempool()

            with open(node.debug_log_path, encoding="utf-8") as log:
                log.seek(log_pos)
                lines = [l for l in log.read().splitlines() if f"UpdateTip: new best={block.hash_hex}" in l]
            assert_equal(len(lines), 1)
            assert "[msghand]" in lines[0], lines[0]


if __name__ == "__main__":
    IBDExitAndTipPathTest(__file__).main()
