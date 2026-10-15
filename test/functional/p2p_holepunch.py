#!/usr/bin/env python3
# Copyright (c) 2026-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test experimental holepunch advertisement admission and relay."""

from copy import deepcopy
import time

from test_framework.key import ECKey
from test_framework.messages import (
    CAddress,
    NODE_P2P_V2,
    msg_addrv2,
    ser_compact_size,
    ser_string,
)
from test_framework.p2p import P2PInterface, P2P_SERVICES, p2p_lock
from test_framework.signet import SIGNET_DEFAULT_CHALLENGE
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, p2p_port

EPOCH_SECONDS = 3600


class AddrReceiver(P2PInterface):
    def __init__(self, *, support_addrv2=True):
        super().__init__(support_addrv2=support_addrv2)
        self.addresses = []
        self.batch_sizes = []

    def on_addrv2(self, message):
        self.addresses.extend(message.addrs)
        self.batch_sizes.append(len(message.addrs))

    def on_addr(self, message):
        self.addresses.extend(message.addrs)

    def received(self):
        with p2p_lock:
            return list(self.addresses)

    def clear(self):
        with p2p_lock:
            self.addresses.clear()
            self.batch_sizes.clear()


class RawAddress:
    """Encode a well-framed addrv2 record with an incorrect payload length."""

    def __init__(self, address, payload):
        self.address = address
        self.payload = payload

    def serialize_v2(self):
        addr = self.address
        return (addr.time.to_bytes(4, "little") + ser_compact_size(addr.nServices)
                + bytes([CAddress.NET_HOLEPUNCH]) + ser_string(self.payload)
                + addr.port.to_bytes(2, "big"))


class HolePunchTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.extra_args = [["-whitelist=addr@127.0.0.1"]]

    def advance(self, seconds):
        self.mocktime += seconds
        self.nodes[0].setmocktime(self.mocktime)
        for peer in self.nodes[0].p2ps:
            peer.sync_with_ping()

    def next_epoch(self):
        self.advance(EPOCH_SECONDS - self.mocktime % EPOCH_SECONDS + 10)

    def descriptor(self, index, **fields):
        addr = CAddress()
        addr.net = CAddress.NET_HOLEPUNCH
        addr.ip = f"123.45.{index // 256}.{index % 256}"
        addr.port = 8333
        addr.time = self.mocktime
        addr.nServices = P2P_SERVICES | NODE_P2P_V2
        addr.holepunch_epoch = self.mocktime // EPOCH_SECONDS
        key = ECKey()
        key.set((index + 1).to_bytes(32, "big"), compressed=True)
        addr.holepunch_key = key.get_pubkey().get_bytes()
        for name, value in fields.items():
            setattr(addr, name, value)
        return addr

    def send(self, addresses):
        message = msg_addrv2()
        message.addrs = addresses
        self.source.send_and_ping(message)

    def flush(self, opportunities=1):
        # Ordinary address sends have a 30-second mean. Advancing ten minutes
        # makes a missed send opportunity negligible, without crossing an epoch.
        for _ in range(opportunities):
            self.advance(600)

    def assert_received(self, expected):
        def identity(addr):
            return (addr.net, addr.ip, addr.port, addr.holepunch_format,
                    addr.holepunch_epoch, addr.holepunch_key)

        assert_equal(sorted(map(identity, self.receiver.received())), sorted(map(identity, expected)))
        assert all(size <= 10 for size in self.receiver.batch_sizes)

    def run_test(self):
        self.mocktime = int(time.time()) // EPOCH_SECONDS * EPOCH_SECONDS + 10
        self.nodes[0].setmocktime(self.mocktime)
        self.source = self.nodes[0].add_p2p_connection(P2PInterface(support_addrv2=True))
        self.receiver = self.nodes[0].add_p2p_connection(AddrReceiver())

        self.test_invalid_records()
        self.test_conflicts_and_epochs()
        self.test_expired_queue()
        self.test_relay_batches()
        self.test_legacy_peer()
        self.test_configuration()

    def test_invalid_records(self):
        self.log.info("Invalid holepunch records do not invalidate good siblings")
        base = self.descriptor(1)
        invalid = [RawAddress(base, bytes(length)) for length in (0, 41, 43, 512)]
        invalid.extend([
            self.descriptor(2, holepunch_format=1),
            self.descriptor(3, holepunch_key=bytes(33)),
            self.descriptor(4, holepunch_key=b"\x02" + b"\xff" * 32),
            self.descriptor(5, holepunch_key=b"\x04" + base.holepunch_key[1:]),
            self.descriptor(6, port=0),
            self.descriptor(7, ip="127.0.0.1"),
            self.descriptor(8, ip="10.0.0.1"),
            self.descriptor(9, ip="192.168.1.1"),
            self.descriptor(10, holepunch_epoch=base.holepunch_epoch - 1),
            self.descriptor(11, holepunch_epoch=base.holepunch_epoch + 1),
        ])
        good = []
        ordinary = CAddress()
        ordinary.ip = "124.45.6.7"
        ordinary.port = 8333
        ordinary.time = self.mocktime
        ordinary.nServices = P2P_SERVICES
        for offset in range(0, len(invalid), 8):
            descriptor = self.descriptor(100 + offset)
            good.append(descriptor)
            siblings = [descriptor, ordinary] if offset == 0 else [descriptor]
            self.send(invalid[offset:offset + 8] + siblings)
        self.flush()
        self.assert_received(good + [ordinary])

        self.log.info("Holepunch descriptors stay out of ordinary address selection")
        assert_equal([(addr["address"], addr["port"]) for addr in self.nodes[0].getnodeaddresses(count=0)], [(ordinary.ip, ordinary.port)])
        self.receiver.clear()

    def test_conflicts_and_epochs(self):
        self.log.info("The first descriptor wins conflicts until its epoch expires")
        self.next_epoch()
        original = self.descriptor(200)
        self.send([original])
        self.flush()
        self.assert_received([original])
        self.receiver.clear()

        duplicate = deepcopy(original)
        duplicate.time += 60
        duplicate.nServices = P2P_SERVICES
        conflict = self.descriptor(201, ip=original.ip)
        different_port = self.descriptor(202, ip=original.ip, port=8334)
        self.send([duplicate, conflict, different_port])
        self.flush()
        self.assert_received([different_port])
        self.receiver.clear()

        self.log.info("Duplicate suppression includes the epoch and public key")
        self.next_epoch()
        replacement = self.descriptor(203, ip=original.ip)
        self.send([original, replacement])
        self.flush()
        self.assert_received([replacement])
        self.receiver.clear()

    def test_expired_queue(self):
        self.log.info("Queued records are rechecked at transmission time")
        self.advance(EPOCH_SECONDS - self.mocktime % EPOCH_SECONDS - 1)
        # The previous advance runs the send timers with empty queues. While
        # mocktime is fixed, the new records wait for their next send opportunity.
        self.send([self.descriptor(300)])
        self.assert_received([])
        self.flush()
        self.assert_received([])

    def test_relay_batches(self):
        self.log.info("Queued announcements are split into batches of at most ten")
        self.next_epoch()
        descriptors = [self.descriptor(400 + index) for index in range(25)]
        for offset in range(0, len(descriptors), 10):
            self.send(descriptors[offset:offset + 10])
        self.flush(opportunities=3)
        self.assert_received(descriptors)
        self.receiver.clear()

    def test_legacy_peer(self):
        self.log.info("Holepunch records are never converted into legacy IPv4 addresses")
        self.next_epoch()
        legacy = self.nodes[0].add_p2p_connection(AddrReceiver(support_addrv2=False))
        legacy.clear()  # Discard its ordinary getaddr response.
        self.send([self.descriptor(500)])
        self.flush()
        assert_equal(legacy.received(), [])

    def test_configuration(self):
        self.log.info("Participation requires an isolated network and a fixed direct IPv4 binding")
        self.nodes[0].disconnect_p2ps()
        self.stop_node(0)
        # The startup smoke test must not dial the public addresses used to
        # exercise relay above.
        (self.nodes[0].chain_path / "peers.dat").unlink()
        binding = f"-holepunchbind=127.0.0.1:{p2p_port(1)}"
        for chain in ("main", "testnet4", "signet"):
            self.nodes[0].assert_start_raises_init_error(
                extra_args=["-noconf", f"-chain={chain}", "-holepunch", binding],
                expected_msg="Error: -holepunch is only supported on regtest or a custom signet",
            )
        self.nodes[0].assert_start_raises_init_error(
            extra_args=["-noconf", "-chain=signet", f"-signetchallenge={SIGNET_DEFAULT_CHALLENGE}", "-holepunch", binding],
            expected_msg="Error: -holepunch is only supported on regtest or a custom signet",
        )
        for invalid_binding in ([], ["-holepunchbind=127.0.0.1:0"], ["-holepunchbind=0.0.0.0:8333"], ["-holepunchbind=[::1]:8333"]):
            self.nodes[0].assert_start_raises_init_error(
                extra_args=["-holepunch"] + invalid_binding,
                expected_msg="Error: -holepunch requires -holepunchbind=<local IPv4>:<nonzero port>",
            )
        self.nodes[0].assert_start_raises_init_error(
            extra_args=[binding], expected_msg="Error: -holepunchbind requires -holepunch",
        )
        self.nodes[0].assert_start_raises_init_error(
            extra_args=["-holepunch", binding, "-v2transport=0"],
            expected_msg="Error: -holepunch requires -v2transport",
        )
        for restriction in ("-onlynet=ipv6", "-proxy=127.0.0.1:1"):
            self.nodes[0].assert_start_raises_init_error(
                extra_args=["-holepunch", binding, "-v2transport=1", restriction],
                expected_msg="Error: -holepunch requires direct IPv4 connectivity",
            )
        self.start_node(0, extra_args=["-holepunch", binding, "-v2transport=1"])


if __name__ == "__main__":
    HolePunchTest(__file__).main()
