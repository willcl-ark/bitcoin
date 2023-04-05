#!/usr/bin/env python3
# Copyright (c) 2023 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""
Test mempool P2P message request limit
"""
import time

from test_framework.messages import (
    COIN,
    msg_mempool,
)
from test_framework.p2p import P2PInterface
from test_framework.test_framework import BitcoinTestFramework
from test_framework.wallet import MiniWallet


class RequestPeer(P2PInterface):
    def __init__(self):
        super().__init__()


class MempoolMsgTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.extra_args = [['-peerbloomfilters', '-whitelist=noban@127.0.0.1'], []]

    def setup_network(self):
        self.setup_nodes()
        self.connect_nodes(0, 1)

    def test_msg_mempool_limit(self):
        self.log.info("Check that a node with bloom filters enabled limits mempool messages")
        request_peer = RequestPeer()

        # Set the mocktime so we can control when the next message will be permitted
        old_time = int(time.time())
        self.nodes[0].setmocktime(old_time)

        # Send a transaction so that we have mempool content to communicate
        self.log.debug("Create a tx relevant to the peer before connecting")
        spk = self.wallet0.get_scriptPubKey()
        txid, _ = self.wallet1.send_to(from_node=self.nodes[1], scriptPubKey=spk, amount=9 * COIN)

        # Request the mempool
        self.log.debug("Send a mempool msg after connecting and check that the tx is received")
        self.nodes[0].add_p2p_connection(request_peer)
        request_peer.send_message(msg_mempool())
        request_peer.wait_for_tx(txid)

        # Move time forward less than MEMPOOL_REPLY_INTERVAL seconds and re-request the mempool
        self.log.debug("Move time forward < MEMPOOL_REPLY_INTERVAL seconds and re-request the mempool")
        self.nodes[0].setmocktime(old_time + 29)
        with self.nodes[0].assert_debug_log(expected_msgs=["[net] mempool request too soon since last"]):
            request_peer.send_message(msg_mempool())

        # Move time forward more than MEMPOOL_REPLY_INTERVAL seconds and re-request the mempool
        self.log.debug("Move time forward > MEMPOOL_REPLY_INTERVAL seconds and re-request the mempool")
        self.nodes[0].setmocktime(old_time + 31)
        request_peer.send_message(msg_mempool())
        request_peer.wait_for_tx(txid)

    def run_test(self):
        self.wallet0 = MiniWallet(self.nodes[0])
        self.wallet1 = MiniWallet(self.nodes[1])
        self.log.info('Test mempool message limit')
        self.test_msg_mempool_limit()


if __name__ == '__main__':
    MempoolMsgTest().main()
