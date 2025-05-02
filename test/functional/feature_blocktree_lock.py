#!/usr/bin/env python3
# Copyright (c) The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A paused block-tree reader must not turn contention into a fatal flush error."""

import concurrent.futures
import time

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal


class BlockTreeLockTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.uses_wallet = False
        self.rpc_timeout = 120

    def skip_test_if_missing_module(self):
        self.skip_if_platform_not_posix()

    def run_test(self):
        import fcntl

        node = self.nodes[0]
        self.generate(node, 1)
        node.gettxoutsetinfo("none")
        self.generate(node, 1)
        tip = node.getbestblockhash()
        lock_path = node.chain_path / "blocks" / "index" / ".lock"
        with concurrent.futures.ThreadPoolExecutor() as executor:
            with lock_path.open("a") as reader_lock:
                fcntl.lockf(reader_lock, fcntl.LOCK_EX)
                # The old access-lock timeout killed the writer after 30 seconds.
                flush = executor.submit(node.gettxoutsetinfo, "none")
                try:
                    time.sleep(31)
                    assert not flush.done()
                    assert not node.is_node_stopped()
                finally:
                    fcntl.lockf(reader_lock, fcntl.LOCK_UN)
            flush.result(timeout=30)
        assert_equal(node.getbestblockhash(), tip)
        self.restart_node(0)
        assert_equal(node.getbestblockhash(), tip)

        self.log.info("Shutdown can interrupt a store-access wait")
        with lock_path.open("a") as reader_lock:
            fcntl.lockf(reader_lock, fcntl.LOCK_EX)
            try:
                self.stop_node(0)
            finally:
                fcntl.lockf(reader_lock, fcntl.LOCK_UN)
        self.start_node(0)
        assert_equal(node.getbestblockhash(), tip)


if __name__ == "__main__":
    BlockTreeLockTest(__file__).main()
