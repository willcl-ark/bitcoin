#!/usr/bin/env python3
# Copyright (c) 2026-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test async undo persistence, reader settlement, disconnect, and restart durability.

During IBD, undo data is written asynchronously by AsyncUndoWriter. Readers that
require undo data (getblock verbosity=2, getblockstats, coinstatsindex, and
DisconnectBlock via invalidateblock) must settle pending undo writes first and
observe consistent undo records across restarts.
"""

import time

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal
from test_framework.wallet import MiniWallet


class AsyncUndoTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 2
        self.extra_args = [[], ["-txindex=1", "-coinstatsindex=1"]]

    def setup_network(self):
        self.setup_nodes()

    def run_test(self):
        source, syncing = self.nodes
        # Keep block timestamps 2 days old so the syncing node remains in IBD while
        # connecting blocks and exercises the async undo path.
        mocktime = int(time.time()) - 2 * 24 * 3600
        source.setmocktime(mocktime)
        wallet = MiniWallet(source)
        self.generate(wallet, 105, sync_fun=self.no_op)

        tx_blocks = []
        for _ in range(5):
            for _ in range(4):
                wallet.send_self_transfer(from_node=source)
            tx_blocks.append(self.generate(source, 1, sync_fun=self.no_op)[0])

        self.log.info("Sync transaction-bearing blocks in IBD so AsyncUndoWriter writes rev*.dat")
        self.connect_nodes(1, 0)
        self.sync_blocks()
        assert syncing.getblockchaininfo()["initialblockdownload"]

        self.log.info("Verify undo-dependent RPCs and indexes settle pending async undo writes")
        for block_hash in tx_blocks:
            verbose = syncing.getblock(block_hash, 2)
            assert all("fee" in tx for tx in verbose["tx"][1:])
            stats = syncing.getblockstats(block_hash)
            assert_equal(stats["txs"], 5)

        syncing.syncwithvalidationinterfacequeue()
        self.wait_until(lambda: syncing.getindexinfo("coinstatsindex")["coinstatsindex"]["synced"])
        assert_equal(
            syncing.gettxoutsetinfo("muhash")["muhash"],
            source.gettxoutsetinfo("muhash")["muhash"],
        )

        self.log.info("Disconnect and reconnect blocks written via async undo")
        self.disconnect_nodes(1, 0)
        tip_hash = tx_blocks[-1]
        syncing.invalidateblock(tx_blocks[-3])
        assert_equal(syncing.getblockcount(), 107)
        syncing.reconsiderblock(tx_blocks[-3])
        assert_equal(syncing.getbestblockhash(), tip_hash)

        self.log.info("Restart node and verify durable chainstate and undo data")
        self.restart_node(1, extra_args=self.extra_args[1])
        assert_equal(syncing.getbestblockhash(), tip_hash)
        assert syncing.verifychain(checklevel=4, nblocks=0)
        assert_equal(
            syncing.gettxoutsetinfo("muhash")["muhash"],
            source.gettxoutsetinfo("muhash")["muhash"],
        )


if __name__ == "__main__":
    AsyncUndoTest(__file__).main()
