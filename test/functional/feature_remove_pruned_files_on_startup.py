#!/usr/bin/env python3
# Copyright (c) 2022 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test removing undeleted pruned blk files on startup."""

import os
from test_framework.test_framework import BitcoinTestFramework

class FeatureRemovePrunedFilesOnStartupTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.extra_args = [["-fastprune", "-prune=1"]]

    def mine_batches(self, blocks):
        n = blocks // 250
        for _ in range(n):
            self.generate(self.nodes[0], 250)
        self.generate(self.nodes[0], blocks % 250)
        self.sync_blocks()

    def run_test(self):
        blk0 = self.nodes[0].blocks_path / "blk00000.dat"
        rev0 = self.nodes[0].blocks_path / "rev00000.dat"
        blk1 = self.nodes[0].blocks_path / "blk00001.dat"
        rev1 = self.nodes[0].blocks_path / "rev00001.dat"
        self.mine_batches(800)
        fo1 = os.open(blk0, os.O_RDONLY)
        fo2 = os.open(rev1, os.O_RDONLY)
        fd1 = os.fdopen(fo1)
        fd2 = os.fdopen(fo2)
        self.nodes[0].pruneblockchain(600)

        # Windows systems will not remove files with an open fd
        if os.name != 'nt':
            assert not blk0.exists()
            assert not rev0.exists()
            assert not blk1.exists()
            assert not rev1.exists()
        else:
            assert blk0.exists()
            assert not rev0.exists()
            assert not blk1.exists()
            assert rev1.exists()

        # Check that the files are removed on restart once the fds are closed
        fd1.close()
        fd2.close()
        self.restart_node(0)
        assert not blk0.exists()
        assert not rev1.exists()

if __name__ == '__main__':
    FeatureRemovePrunedFilesOnStartupTest().main()
