#!/usr/bin/env python3
# Copyright (c) 2026 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/licenses/mit-license.php.

"""Test board isolation, verified announcements and a coordinator-less v2 connection."""

import socket
import time

from test_framework.key import ECKey
from test_framework.messages import (
    NODE_P2P_V2,
    deser_compact_size,
    deser_string,
    msg_feature,
    msg_getaddr,
    msg_sendaddrv2,
    msg_verack,
    ser_compact_size,
    ser_string,
)
from test_framework.p2p import MESSAGEMAP, P2PInterface, p2p_lock
from test_framework.test_framework import BitcoinTestFramework, SkipTest
from test_framework.util import assert_equal, p2p_port

FEATURE_ID = "https://delvingbitcoin.org/t/2497/22"


class RawAddrV2:
    msgtype = b"addrv2"

    def __init__(self, entries=()):
        self.entries = list(entries)

    def serialize(self):
        result = ser_compact_size(len(self.entries))
        for timestamp, services, network, address, port in self.entries:
            result += timestamp.to_bytes(4, "little") + ser_compact_size(services)
            result += bytes([network]) + ser_string(address) + port.to_bytes(2, "big")
        return result

    def deserialize(self, stream):
        self.entries = []
        for _ in range(deser_compact_size(stream)):
            timestamp = int.from_bytes(stream.read(4), "little")
            services = deser_compact_size(stream)
            network = stream.read(1)[0]
            address = deser_string(stream)
            port = int.from_bytes(stream.read(2), "big")
            self.entries.append((timestamp, services, network, address, port))


class Visitor(P2PInterface):
    def peer_connect_send_version(self, services):
        super().peer_connect_send_version(services)
        self.on_connection_send_msg.relay = False

    def on_version(self, message):
        self.nServices = message.nServices
        self.send_without_ping(msg_sendaddrv2())
        self.send_without_ping(msg_feature(FEATURE_ID, b"\x00" + (2).to_bytes(4, "little")))
        self.send_without_ping(msg_verack())

    def on_addrv2(self, message):
        pass


class HolePunchTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 4
        self.extra_args = [
            ["-v2transport=1", "-holepunchboard", f"-bind=127.0.0.{10 + i}:{p2p_port(i)}"]
            for i in range(2)
        ] + [
            ["-v2transport=1", "-holepunch", "-onlynet=ipv4",
             f"-bind=127.0.0.{i}:{p2p_port(i)}", f"-holepunchbind=127.0.0.{i}",
             f"-holepunchport={p2p_port(i) + 5000}"]
            for i in range(2, 4)
        ]

    def skip_test_if_missing_module(self):
        if not hasattr(socket, "SO_REUSEPORT"):
            raise SkipTest("TCP holepunching requires SO_REUSEPORT")
        try:
            for address in ("127.0.0.2", "127.0.0.3", "127.0.0.10", "127.0.0.11"):
                with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
                    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
                    sock.bind((address, 0))
        except OSError as error:
            raise SkipTest(f"TCP holepunching requires reusable sockets on distinct loopback addresses: {error}")

    def setup_network(self):
        self.setup_nodes()

    def run_test(self):
        MESSAGEMAP[b"addrv2"] = RawAddrV2
        self.test_board()
        expected_logs = [
            "Board handshake complete, role=visitor",
            "external endpoint confirmed by quorum",
            "Received exact registration acknowledgement",
            "Round height=",
            "Session started role=",
            "Punched v2 handshake complete, role=",
        ]
        with self.nodes[2].assert_debug_log(expected_logs, timeout=5), self.nodes[3].assert_debug_log(expected_logs, timeout=5):
            self.test_punch()

    def test_board(self):
        board = self.nodes[0]
        peer = board.add_p2p_connection(Visitor(), dstaddr="127.0.0.10")
        with p2p_lock:
            assert_equal(peer.message_count["version"], 1)
            assert_equal(peer.last_message["feature"].feature_id, FEATURE_ID)
            assert_equal(peer.last_message["feature"].feature_data, b"\x00\x01\x00\x00\x00")
            observed = peer.last_message["version"].addrTo
        key = ECKey()
        key.set((1).to_bytes(32, "big"), compressed=True)
        public_key = key.get_pubkey().get_bytes()
        address = b"\x01" + socket.inet_aton(observed.ip) + public_key
        good = (int(time.time()), NODE_P2P_V2, 9, address, observed.port)

        self.log.info("A board ignores announcements that do not match the TCP endpoint")
        peer.send_and_ping(RawAddrV2([good[:-1] + (observed.port + 1,)]))
        with p2p_lock:
            assert_equal(peer.message_count["addrv2"], 0)
        peer.send_without_ping(RawAddrV2([good]))
        peer.wait_until(lambda: peer.message_count["addrv2"] == 1)
        with p2p_lock:
            echo = peer.last_message["addrv2"].entries
            assert_equal(len(echo), 1)
            assert_equal(echo[0][1:], good[1:])
            for forbidden in ("wtxidrelay", "sendcmpct", "getaddr", "inv", "headers"):
                assert_equal(peer.message_count[forbidden], 0)
        assert_equal(board.getnodeaddresses(), [])

        self.log.info("Ordinary messages close board connections")
        peer.send_without_ping(msg_getaddr())
        peer.wait_for_disconnect()
        board.disconnect_p2ps()
        self.wait_until(lambda: not board.getpeerinfo())

        self.log.info("PUNCHKEY on an ordinary connection closes it before addrman insertion")
        ordinary = board.add_p2p_connection(P2PInterface(), dstaddr="127.0.0.10")
        ordinary.send_without_ping(RawAddrV2([good]))
        ordinary.wait_for_disconnect()
        board.disconnect_p2ps()
        self.wait_until(lambda: not board.getpeerinfo())
        assert_equal(board.getnodeaddresses(), [])

    def test_punch(self):
        self.log.info("Two visitors use two boards and their fixed source ports")
        now = int(time.time())
        for node in self.nodes:
            node.setmocktime(now)
        for node in self.nodes[2:]:
            for i in range(2):
                node.addconnection(f"127.0.0.{10 + i}:{p2p_port(i)}", "holepunch-board", True)

        def registered(node):
            peers = node.getpeerinfo()
            return len(peers) == 2 and all(
                p["connection_type"] == "holepunch-board" and
                p["transport_protocol_type"] == "v2" and
                p.get("bytesrecv_per_msg", {}).get("addrv2", 0) >= 90
                for p in peers
            )

        for node in self.nodes[2:]:
            self.wait_until(lambda node=node: registered(node))
            for peer in node.getpeerinfo():
                assert_equal(peer["addrbind"], f"127.0.0.{node.index}:{p2p_port(node.index) + 5000}")
            # A received-byte counter precedes message processing. A subsequent
            # round trip places processing of these announcements before the block.
            ping_bytes = {p["id"]: p.get("bytessent_per_msg", {}).get("ping", 0) for p in node.getpeerinfo()}
            node.ping()
            self.wait_until(lambda node=node, ping_bytes=ping_bytes: all(
                p.get("bytessent_per_msg", {}).get("ping", 0) > ping_bytes[p["id"]] and "pingwait" not in p
                for p in node.getpeerinfo()
            ))

        self.log.info("An announcement waits for the next block's parent before it can be picked")
        for offset in (1, 2):
            for node in self.nodes:
                node.setmocktime(now + offset)
            block_hash = self.generate(self.nodes[2], 1, sync_fun=self.no_op)[0]
            self.nodes[3].submitblock(self.nodes[2].getblock(block_hash, 0))
            if offset == 1:
                for node in self.nodes[2:]:
                    assert_equal(len(node.getpeerinfo()), 2)

        def punched(node):
            peers = [p for p in node.getpeerinfo() if p["connection_type"] != "holepunch-board"]
            return len(peers) == 1 and peers[0]["transport_protocol_type"] == "v2" and peers[0].get("bytessent_per_msg", {}).get("ping", 0) > 0

        self.wait_until(lambda: all(punched(node) for node in self.nodes[2:]))
        for node in self.nodes[2:]:
            peers = [p for p in node.getpeerinfo() if p["connection_type"] != "holepunch-board"]
            assert_equal(len(peers), 1)
            peer = peers[0]
            assert_equal(peer["transport_protocol_type"], "v2")
            assert_equal(peer["addr"], f"127.0.0.{5 - node.index}:{p2p_port(5 - node.index) + 5000}")
            assert_equal(peer["addrbind"], f"127.0.0.{node.index}:{p2p_port(node.index) + 5000}")
        outbound = [p for node in self.nodes[2:] for p in node.getpeerinfo() if p["connection_type"] == "holepunch"]
        assert_equal(len(outbound), 1)
        assert_equal(outbound[0]["inbound"], False)


if __name__ == "__main__":
    HolePunchTest(__file__).main()
