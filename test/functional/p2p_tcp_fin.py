#!/usr/bin/env python3
# Copyright (c) 2026 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test how a clean TCP EOF affects queued P2P messages."""

import socket

from test_framework.messages import (
    MAGIC_BYTES,
    hash256,
    msg_ping,
    msg_verack,
    msg_version,
)
from test_framework.p2p import P2P_VERSION
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, p2p_port
from test_framework.wallet import MiniWallet


def frame(command, payload=b""):
    assert len(command) <= 12
    return (MAGIC_BYTES["regtest"] + command.ljust(12, b"\x00")
            + len(payload).to_bytes(4, "little") + hash256(payload)[:4] + payload)


def recv_exact(sock, size):
    result = bytearray()
    while len(result) < size:
        chunk = sock.recv(size - len(result))
        if not chunk:
            raise ConnectionError("peer closed while reading a P2P message")
        result.extend(chunk)
    return bytes(result)


def recv_message(sock):
    header = recv_exact(sock, 24)
    assert_equal(header[:4], MAGIC_BYTES["regtest"])
    length = int.from_bytes(header[16:20], "little")
    payload = recv_exact(sock, length)
    assert_equal(header[20:24], hash256(payload)[:4])
    return header[4:16].rstrip(b"\x00"), payload


class TcpFinTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

    def connect_raw_peer(self):
        sock = socket.create_connection(("127.0.0.1", p2p_port(0)), timeout=10)
        version = msg_version()
        version.nVersion = P2P_VERSION
        version.relay = 1
        sock.sendall(frame(version.msgtype, version.serialize()))
        while True:
            command, _ = recv_message(sock)
            if command == b"version":
                break
        sock.sendall(frame(msg_verack.msgtype))
        while True:
            command, _ = recv_message(sock)
            if command == b"verack":
                break
        peer_id = self.nodes[0].getpeerinfo()[-1]["id"]
        return sock, peer_id

    def wait_for_peer_removal(self, peer_id):
        self.wait_until(lambda: peer_id not in [p["id"] for p in self.nodes[0].getpeerinfo()])

    def run_test(self):
        node = self.nodes[0]
        wallet = MiniWallet(node)
        self.generate(wallet, 101)
        control_tx = wallet.create_self_transfer()
        fin_tx = wallet.create_self_transfer()
        for tx in (control_tx, fin_tx):
            assert_equal(node.testmempoolaccept([tx["hex"]])[0]["allowed"], True)
            assert tx["txid"] not in node.getrawmempool()

        self.log.info("Check that the raw peer and transaction are valid")
        sock, peer_id = self.connect_raw_peer()
        with sock:
            nonce = 12345
            sock.sendall(frame(b"tx", bytes.fromhex(control_tx["hex"]))
                         + frame(b"ping", msg_ping(nonce).serialize()))
            while True:
                command, payload = recv_message(sock)
                if command == b"pong" and int.from_bytes(payload, "little") == nonce:
                    break
            sock.shutdown(socket.SHUT_WR)
            self.wait_for_peer_removal(peer_id)
        assert control_tx["txid"] in node.getrawmempool()

        self.log.info("Check that a queued transaction is dropped after TCP FIN")
        sock, peer_id = self.connect_raw_peer()
        with sock:
            # A single write keeps the transaction immediately before FIN on the
            # byte stream. TCP does not guarantee they share a network packet.
            queued = frame(b"unknown") * 2000
            sock.sendall(queued + frame(b"tx", bytes.fromhex(fin_tx["hex"])))
            sock.shutdown(socket.SHUT_WR)
            self.wait_for_peer_removal(peer_id)
        assert fin_tx["txid"] not in node.getrawmempool()


if __name__ == "__main__":
    TcpFinTest(__file__).main()
