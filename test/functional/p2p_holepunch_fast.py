#!/usr/bin/env python3
# Copyright (c) 2026-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Establish a direct, authenticated connection through fast-profile gossip."""

import socket
import time

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, p2p_port


class HolePunchFastTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 3
        self.punch_endpoints = [f"127.0.0.1:{p2p_port(index + self.num_nodes)}" for index in range(2)]
        self.extra_args = [
            ["-holepunchfast", "-holepunch", "-v2transport=1",
             f"-holepunchbind={endpoint}", f"-holepunchexternal={endpoint}"]
            for endpoint in self.punch_endpoints
        ] + [["-holepunchfast", "-v2transport=1"]]

    def setup_network(self):
        self.setup_nodes()
        self.connect_nodes(0, 2)
        self.connect_nodes(1, 2)
        self.generate(self.nodes[2], 1)

    @staticmethod
    def punched_peers(node):
        # PONG is processed only after proof verification and verack completion.
        return [peer for peer in node.getpeerinfo()
                if peer["connection_type"] == "holepunch" and peer["bytesrecv_per_msg"].get("pong", 0)]

    def run_test(self):
        a, b, relay = self.nodes
        manual_ids = [node.getpeerinfo()[0]["id"] for node in (a, b)]
        for node in (a, b):
            assert_equal([peer["connection_type"] for peer in node.getpeerinfo()], ["manual"])

        self.log.info("The optional punch listener closes sockets outside an active attempt")
        for endpoint in self.punch_endpoints:
            host, port = endpoint.split(":")
            try:
                sock = socket.create_connection((host, int(port)), timeout=5)
            except ConnectionRefusedError:
                # Sharing a listener and active sockets on the binding is an
                # optional operating-system capability.
                continue
            with sock:
                assert_equal(sock.recv(1), b"")
        for node in (a, b):
            assert_equal(len(node.getpeerinfo()), 1)

        self.log.info("Wait for A and B to discover each other through C and establish a v2 holepunch connection")
        # Use actual wall time for the shared schedule and monotonic deadlines.
        # Startup waits for the next 60-second epoch; origination starts at
        # second 5 and the candidate snapshot is taken at second 30. Additional
        # epochs allow the operating system to reject an individual TCP attempt.
        self.wait_until(lambda: len(self.punched_peers(a)) == len(self.punched_peers(b)) == 1,
                        timeout=180, check_interval=0.1)
        punched = [self.punched_peers(node)[0] for node in (a, b)]
        for index, node in enumerate((a, b)):
            peer = punched[index]
            assert_equal(peer["transport_protocol_type"], "v2")
            assert_equal(peer["addr"], self.punch_endpoints[1 - index])
            assert_equal(peer["addrbind"], self.punch_endpoints[index])
            assert peer["bytesrecv_per_msg"].get("feature", 0) > 0
            assert peer["bytessent_per_msg"].get("feature", 0) > 0
            peers = node.getpeerinfo()
            assert_equal(len(peers), 2)
            assert_equal([p["id"] for p in peers if p["connection_type"] == "manual"], [manual_ids[index]])
            assert all(p["connection_type"] != "holepunch-observer" for p in peers)
            assert all(f"{addr['address']}:{addr['port']}" not in self.punch_endpoints
                       for addr in node.getnodeaddresses(count=0))
        assert_equal(len(relay.getpeerinfo()), 2)
        assert all(peer["connection_type"] == "inbound" for peer in relay.getpeerinfo())

        self.log.info("The authenticated connection survives descriptor expiry and relays without C")
        self.disconnect_nodes(0, 2)
        self.disconnect_nodes(1, 2)
        assert_equal(relay.getpeerinfo(), [])
        relay_tip = relay.getbestblockhash()
        # Changing wall time after authentication expires both descriptors and
        # cancels unfinished attempts, but must preserve admitted connections.
        expired_time = int(time.time()) + 120
        for node in (a, b):
            node.setmocktime(expired_time)
        pong_before = punched[0]["bytesrecv_per_msg"]["pong"]
        a.ping()
        self.wait_until(lambda: self.punched_peers(a)[0]["bytesrecv_per_msg"]["pong"] > pong_before)
        self.generate(a, 1, sync_fun=lambda: self.sync_blocks([a, b]))
        assert_equal(a.getbestblockhash(), b.getbestblockhash())
        assert_equal(relay.getbestblockhash(), relay_tip)
        for index, node in enumerate((a, b)):
            assert_equal([peer["id"] for peer in node.getpeerinfo()], [punched[index]["id"]])


if __name__ == "__main__":
    HolePunchFastTest(__file__).main()
