#!/usr/bin/env python3
# Copyright (c) 2026-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test bounded backup requests during IBD."""

import time

from test_framework.blocktools import create_block
from test_framework.messages import CBlockHeader, CInv, MSG_BLOCK, MSG_TYPE_MASK, msg_block, msg_headers, msg_notfound
from test_framework.p2p import P2PDataStore, P2PInterface
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error


class WithholdingPeer(P2PDataStore):
    def __init__(self, blocks, withheld):
        super().__init__()
        self.block_store = {block.hash_int: block for block in blocks}
        self.last_block_hash = blocks[-1].hash_int
        self.withheld = set(withheld)

    def on_getdata(self, message):
        for inv in message.inv:
            self.getdata_requests.append(inv.hash)
            if inv.type & MSG_TYPE_MASK == MSG_BLOCK and inv.hash not in self.withheld:
                self.send_without_ping(msg_block(self.block_store[inv.hash]))

    def on_getheaders(self, message):
        # Announce only when the test is ready to assign requests.
        pass


class IBDBackupTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.mocktime = 0
        self.extra_args = [["-connect=0", "-dnsseed=0"]]

    def make_blocks(self, count, *, historical_bodies=False):
        node = self.nodes[0]
        # Use a different chain after invalidating the previous scenario.
        self.mocktime = max(int(time.time()), self.mocktime + count + 10)
        node.setmocktime(self.mocktime)
        previous = int(node.getbestblockhash(), 16)
        height = node.getblockcount()
        blocks = []
        for i in range(1, count + 1):
            block_time = node.mocktime + i
            if historical_bodies and i < count:
                block_time -= 2 * 24 * 60 * 60
            block = create_block(previous, height=height + i, ntime=block_time)
            block.solve()
            blocks.append(block)
            previous = block.hash_int
        return blocks

    def add_peer(self, blocks, withheld, index, connection_type="outbound-full-relay"):
        peer = self.nodes[0].add_outbound_p2p_connection(
            WithholdingPeer(blocks, withheld), p2p_idx=index, connection_type=connection_type)
        peer.send_and_ping(msg_headers([CBlockHeader(block) for block in blocks]))
        return peer

    def run_test(self):
        self.test_backup_winner()
        self.restart_node(0)
        self.test_backup_disconnect()
        for notfound in (False, True):
            self.restart_node(0)
            self.test_backup_debt(notfound=notfound)
        self.restart_node(0)
        self.test_backup_stalling()
        self.restart_node(0, extra_args=self.extra_args[0] + ["-maxconnections=13"])
        self.test_temporary_peers(count=2)
        self.restart_node(0, extra_args=self.extra_args[0] + ["-maxconnections=12"])
        self.test_temporary_peers(count=1)

    def test_backup_winner(self):
        self.log.info("A backup wins the missing frontier block; the original response may arrive late")
        node = self.nodes[0]
        blocks = self.make_blocks(3)
        missing = blocks[0].hash_int
        primary = self.add_peer(blocks, [missing], 0)
        primary.wait_until(lambda: len(primary.getdata_requests) == len(blocks))
        self.wait_until(lambda: sum(len(p["inflight"]) for p in node.getpeerinfo()) == 1)
        backup = self.add_peer(blocks, [missing], 1)
        assert_equal(backup.getdata_requests, [])
        assert_equal(node.getblockcount(), 0)

        node.setmocktime(node.mocktime + 1)
        backup.sync_with_ping()
        backup.wait_until(lambda: missing in backup.getdata_requests)
        assert_equal(backup.getdata_requests, [missing])
        assert_equal(sum(len(p["inflight"]) for p in node.getpeerinfo()), 2)
        backup.send_and_ping(msg_block(blocks[0]))
        self.wait_until(lambda: node.getblockcount() == len(blocks))
        assert_equal([p["inflight"] for p in node.getpeerinfo()], [[], []])

        primary.send_and_ping(msg_block(blocks[0]))
        assert_equal(node.getblockcount(), len(blocks))
        assert_equal(node.num_test_p2p_connections(), 2)
        node.invalidateblock(blocks[0].hash_hex)

    def test_backup_disconnect(self):
        self.log.info("Disconnecting a withholding backup does not retry the same frontier")
        node = self.nodes[0]
        initial_height = node.getblockcount()
        blocks = self.make_blocks(3)
        missing = blocks[0].hash_int
        primary = self.add_peer(blocks, [missing], 0)
        primary.wait_until(lambda: len(primary.getdata_requests) == len(blocks))
        self.wait_until(lambda: sum(len(p["inflight"]) for p in node.getpeerinfo()) == 1)
        backup = self.add_peer(blocks, [missing], 1)
        node.setmocktime(node.mocktime + 1)
        backup.sync_with_ping()
        backup.wait_until(lambda: missing in backup.getdata_requests)
        existing_peer_ids = {peer["id"] for peer in node.getpeerinfo()}
        third = self.add_peer(blocks, [missing], 2)
        third_peer_id, = {peer["id"] for peer in node.getpeerinfo()} - existing_peer_ids
        assert_raises_rpc_error(-1, "Block has outstanding IBD backup responses",
                                node.getblockfrompeer, blocks[0].hash_hex, third_peer_id)
        node.setmocktime(node.mocktime + 2)
        third.sync_with_ping()
        assert_equal(third.getdata_requests, [])
        backup.peer_disconnect()
        backup.wait_for_disconnect()
        self.wait_until(lambda: len(node.getpeerinfo()) == 2)
        node.setmocktime(node.mocktime + 2)
        third.sync_with_ping()
        assert_equal(third.getdata_requests, [])
        assert_equal(node.getblockcount(), initial_height)
        primary.send_and_ping(msg_block(blocks[0]))
        self.wait_until(lambda: node.getblockcount() == initial_height + len(blocks))
        node.invalidateblock(blocks[0].hash_hex)

    def test_backup_debt(self, *, notfound):
        self.log.info("Four outstanding original copies bound backup requests until a response settles debt")
        node = self.nodes[0]
        blocks = self.make_blocks(7, historical_bodies=True)
        hashes = [block.hash_int for block in blocks]
        primary = self.add_peer(blocks, hashes[:5], 0)
        primary.wait_until(lambda: len(primary.getdata_requests) == len(blocks))
        self.wait_until(lambda: sum(len(p["inflight"]) for p in node.getpeerinfo()) == 5)
        backup = self.add_peer(blocks, [], 1)
        assert_equal(backup.getdata_requests, [])

        for height in range(1, 5):
            node.setmocktime(node.mocktime + 1)
            backup.sync_with_ping()
            self.wait_until(lambda: node.getblockcount() == height)
            assert_equal(backup.getdata_requests, hashes[:height])
        assert_equal([p["inflight"] for p in node.getpeerinfo()], [[5], []])

        node.setmocktime(node.mocktime + 10)
        backup.sync_with_ping()
        assert_equal(node.getblockcount(), 4)
        assert_equal(backup.getdata_requests, hashes[:4])
        if notfound:
            primary.send_and_ping(msg_notfound([CInv(MSG_BLOCK, hashes[0])]))
        else:
            primary.send_and_ping(msg_block(blocks[0]))
        backup.sync_with_ping()
        self.wait_until(lambda: node.getblockcount() == len(blocks))
        assert_equal(backup.getdata_requests, hashes[:5])
        assert_equal(primary.getdata_requests, hashes)

        for block in blocks[1:5]:
            primary.send_without_ping(msg_block(block))
        primary.sync_with_ping()
        assert_equal([p["inflight"] for p in node.getpeerinfo()], [[], []])
        assert_equal(node.num_test_p2p_connections(), 2)
        node.invalidateblock(blocks[0].hash_hex)

    def test_backup_stalling(self):
        self.log.info("A held backup preserves the ordinary two-second stalling timeout")
        node = self.nodes[0]
        blocks = self.make_blocks(1025)
        missing = blocks[0].hash_int
        primary = self.add_peer(blocks[:1024], [missing], 0)
        primary.wait_until(lambda: len(primary.getdata_requests) == 1024)
        self.wait_until(lambda: sum(len(p["inflight"]) for p in node.getpeerinfo()) == 1)
        backup = self.add_peer(blocks[:1024], [missing], 1)
        backup.block_store[blocks[-1].hash_int] = blocks[-1]
        node.setmocktime(node.mocktime + 1)
        backup.sync_with_ping()
        backup.wait_until(lambda: missing in backup.getdata_requests)
        # Only the backup request remains for this peer. It must still be able
        # to mark the original peer as stalling when the window is exhausted.
        with node.assert_debug_log(expected_msgs=["Stall started"]):
            backup.send_and_ping(msg_headers([CBlockHeader(block) for block in blocks]))
        node.setmocktime(node.mocktime + 3)
        primary.wait_for_disconnect()
        assert_equal(backup.is_connected, True)
        node.invalidateblock(blocks[0].hash_hex)


    def test_temporary_peers(self, *, count):
        self.log.info("Temporary IBD peers have shallow queues and close when the chain catches up")
        node = self.nodes[0]
        initial_height = node.getblockcount()
        blocks = self.make_blocks(20)
        hashes = [block.hash_int for block in blocks]
        regular = self.add_peer(blocks, hashes, 0)
        regular.wait_until(lambda: len(regular.getdata_requests) == 16)
        if count == 2:
            # Manual connections do not consume automatic outbound capacity.
            node.add_outbound_p2p_connection(P2PInterface(), p2p_idx=3, connection_type="manual")
        ordinary_ids = {p["id"] for p in node.getpeerinfo()}
        ibd_peers = [self.add_peer(blocks, hashes, i + 1, "ibd") for i in range(count)]
        for peer in ibd_peers:
            peer.wait_until(lambda: len(peer.getdata_requests) == 2)
        info = node.getpeerinfo()
        temporary_info = [p for p in info if p["connection_type"] == "ibd"]
        assert_equal(len(temporary_info), count)
        for peer in temporary_info:
            assert_equal(peer["relaytxes"], False)
            assert_equal(peer["addr_relay_enabled"], False)
            assert_equal(len(peer["inflight"]), 2)
        assert_raises_rpc_error(-34, "Already at capacity", node.addconnection,
                                "127.0.0.1:1", "ibd", node.use_v2transport)

        for block in blocks:
            regular.send_without_ping(msg_block(block))
        self.wait_until(lambda: node.getblockcount() == initial_height + len(blocks))
        for peer in ibd_peers:
            peer.wait_for_disconnect()
        regular.sync_with_ping()
        remaining = node.getpeerinfo()
        assert_equal({p["id"] for p in remaining}, ordinary_ids)
        assert_equal([p["connection_type"] for p in remaining],
                     ["outbound-full-relay", "manual"] if count == 2 else ["outbound-full-relay"])
        node.invalidateblock(blocks[0].hash_hex)


if __name__ == '__main__':
    IBDBackupTest(__file__).main()
