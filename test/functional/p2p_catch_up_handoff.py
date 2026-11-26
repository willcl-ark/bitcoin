#!/usr/bin/env python3
# Copyright (c) 2026-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""
Test that a node out of initial block download catches up in the download mode.

The initial block download latch falls once and never rises again, so a node cut off from the network while it
ran stays out of it however far it falls behind. More than a day of blocks behind its best header it processes
blocks in the download mode all the same (node/sync_mode.h): a block it asked for goes to the connect pool. Near
the tip again, a block stays on the message handler.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

# More than CATCH_UP_ENTER_BLOCKS (144).
BLOCKS_BEHIND = 200


class P2PCatchUpHandoffTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        # Several connect pool workers, without which nothing is handed off.
        self.extra_args = [[], ["-prevoutfetchthreads=4"]]

    def run_test(self):
        node0, node1 = self.nodes
        self.generate(node0, 10)
        assert_equal(node1.getblockchaininfo()["initialblockdownload"], False)

        self.log.info("A node out of initial block download falls more than a day of blocks behind")
        self.disconnect_nodes(0, 1)
        self.generate(node0, BLOCKS_BEHIND, sync_fun=self.no_op)

        self.log.info("Back on the network it catches up in the download mode: blocks go to the connect pool")
        with node1.assert_debug_log(expected_msgs=["goes to the connect pool"]):
            self.connect_nodes(0, 1)
            self.sync_blocks()
        assert_equal(node1.getblockchaininfo()["initialblockdownload"], False)

        self.log.info("Near the tip again, a new block stays on the message handler")
        with node1.assert_debug_log(expected_msgs=[], unexpected_msgs=["goes to the connect pool"]):
            self.generate(node0, 1)


if __name__ == '__main__':
    P2PCatchUpHandoffTest(__file__).main()
