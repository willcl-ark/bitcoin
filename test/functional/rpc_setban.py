#!/usr/bin/env python3
# Copyright (c) 2015-2021 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the setban rpc call."""

from contextlib import ExitStack
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    p2p_port,
    assert_equal,
)

class SetBanTests(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [[],[]]

    def is_banned(self, node, addr):
        return any(e['address'] == addr for e in node.listbanned())

    def run_test(self):
        # Node 0 connects to Node 1, check that the noban permission is not granted
        self.connect_nodes(0, 1)
        peerinfo = self.nodes[1].getpeerinfo()[0]
        assert not "noban" in peerinfo["permissions"]

        # Node 0 get banned by Node 1
        self.nodes[1].setban("127.0.0.1", "add")

        # Node 0 should not be able to reconnect
        context = ExitStack()
        context.enter_context(self.nodes[1].assert_debug_log(expected_msgs=['dropped (banned)\n'], timeout=50))
        # When disconnected right after connecting, a v2 node will attempt to reconnect with v1.
        # Wait for that to happen so that it cannot mess with later tests.
        if self.options.v2transport:
            context.enter_context(self.nodes[0].assert_debug_log(expected_msgs=['trying v1 connection'], timeout=50))
        with context:
            self.restart_node(1, [])
            self.nodes[0].addnode("127.0.0.1:" + str(p2p_port(1)), "onetry")

        # However, node 0 should be able to reconnect if it has noban permission
        self.restart_node(1, ['-whitelist=127.0.0.1'])
        self.connect_nodes(0, 1)
        peerinfo = self.nodes[1].getpeerinfo()[0]
        assert "noban" in peerinfo["permissions"]

        # If we remove the ban, Node 0 should be able to reconnect even without noban permission
        self.nodes[1].setban("127.0.0.1", "remove")
        self.restart_node(1, [])
        self.connect_nodes(0, 1)
        peerinfo = self.nodes[1].getpeerinfo()[0]
        assert not "noban" in peerinfo["permissions"]

        self.log.info("Test that a non-IP address can be banned/unbanned")
        node = self.nodes[1]
        tor_addr = "pg6mmjiyjmcrsslvykfwnntlaru7p5svn6y2ymmju6nubxndf4pscryd.onion"
        ip_addr = "1.2.3.4"
        assert not self.is_banned(node, tor_addr)
        assert not self.is_banned(node, ip_addr)

        node.setban(tor_addr, "add")
        assert self.is_banned(node, tor_addr)
        assert not self.is_banned(node, ip_addr)

        node.setban(tor_addr, "remove")
        assert not self.is_banned(self.nodes[1], tor_addr)
        assert not self.is_banned(node, ip_addr)

        self.log.info("Test -bantime")
        self.restart_node(1, ["-bantime=1234"])
        self.nodes[1].setban("127.0.0.1", "add")
        banned = self.nodes[1].listbanned()[0]
        assert_equal(banned['ban_duration'], 1234)

        self.log.info("Test AS banning functionality")
        self.test_as_banning()

    def test_as_banning(self):
        """Test AS (Autonomous System) banning functionality"""
        node = self.nodes[1]

        # Clear any existing bans
        node.clearbanned()
        assert_equal(len(node.listbanned()), 0)

        self.log.info("Test basic AS ban add/remove")
        # Test banning a valid AS number
        as_number = "AS1234"
        node.setban(as_number, "add")

        # Check that the AS is in the banned list
        banned_list = node.listbanned()
        assert_equal(len(banned_list), 1)
        assert_equal(banned_list[0]['address'], as_number)
        assert 'ban_created' in banned_list[0]
        assert 'banned_until' in banned_list[0]
        assert 'ban_duration' in banned_list[0]
        assert 'time_remaining' in banned_list[0]

        # Test removing the AS ban
        node.setban(as_number, "remove")
        assert_equal(len(node.listbanned()), 0)

        self.log.info("Test AS ban case insensitivity")
        # Test that both "AS1234" and "as1234" work
        node.setban("as5678", "add")
        banned_list = node.listbanned()
        assert_equal(len(banned_list), 1)
        assert_equal(banned_list[0]['address'], "AS5678")  # JSON serialization uses uppercase
        node.setban("AS5678", "remove")  # Remove using uppercase
        assert_equal(len(node.listbanned()), 0)

        self.log.info("Test AS ban with custom ban time")
        # Test AS ban with specific ban duration
        node.setban("AS9999", "add", 3600)  # 1 hour
        banned_list = node.listbanned()
        assert_equal(len(banned_list), 1)
        assert_equal(banned_list[0]['address'], "AS9999")
        assert_equal(banned_list[0]['ban_duration'], 3600)
        node.setban("AS9999", "remove")

        self.log.info("Test multiple AS bans")
        # Test multiple AS bans at once
        node.setban("AS1111", "add")
        node.setban("AS2222", "add")
        node.setban("AS3333", "add")

        banned_list = node.listbanned()
        assert_equal(len(banned_list), 3)
        banned_addresses = {ban['address'] for ban in banned_list}
        assert_equal(banned_addresses, {"AS1111", "AS2222", "AS3333"})

        # Test clearbanned removes all AS bans
        node.clearbanned()
        assert_equal(len(node.listbanned()), 0)

        self.log.info("Test mixed IP and AS bans")
        # Test that IP and AS bans can coexist
        node.setban("192.168.1.0/24", "add")
        node.setban("AS4444", "add")

        banned_list = node.listbanned()
        assert_equal(len(banned_list), 2)
        banned_addresses = {ban['address'] for ban in banned_list}
        assert "192.168.1.0/24" in banned_addresses
        assert "AS4444" in banned_addresses

        # Remove just the AS ban
        node.setban("AS4444", "remove")
        banned_list = node.listbanned()
        assert_equal(len(banned_list), 1)
        assert_equal(banned_list[0]['address'], "192.168.1.0/24")

        # Remove the IP ban
        node.setban("192.168.1.0/24", "remove")
        assert_equal(len(node.listbanned()), 0)

        self.log.info("Test AS ban persistence across restarts")
        # Test that AS bans persist across node restarts
        node.setban("AS7777", "add", 86400)  # 24 hour ban
        node.setban("AS8888", "add", 86400)  # 24 hour ban

        # Verify bans are active
        banned_list = node.listbanned()
        assert_equal(len(banned_list), 2)
        banned_addresses = {ban['address'] for ban in banned_list}
        assert_equal(banned_addresses, {"AS7777", "AS8888"})

        # Restart the node
        self.restart_node(1, [])

        # Verify bans survived the restart
        banned_list = node.listbanned()
        assert_equal(len(banned_list), 2)
        banned_addresses = {ban['address'] for ban in banned_list}
        assert_equal(banned_addresses, {"AS7777", "AS8888"})

        # Clean up
        node.clearbanned()
        assert_equal(len(node.listbanned()), 0)

        self.log.info("Test AS ban error cases")
        # Test invalid AS numbers
        try:
            node.setban("AS0", "add")  # AS0 is reserved and invalid
            assert False, "Should have failed to ban AS0"
        except Exception as e:
            assert "Invalid AS number" in str(e)

        try:
            node.setban("AS99999999999", "add")  # Too large AS number
            assert False, "Should have failed to ban large AS number"
        except Exception as e:
            assert "Invalid AS number" in str(e)

        try:
            node.setban("AS", "add")  # Missing AS number
            assert False, "Should have failed to ban incomplete AS"
        except Exception as e:
            # This falls back to IP/subnet validation, so expect that error
            assert ("Invalid AS number" in str(e) or "Invalid IP/Subnet" in str(e))

        try:
            node.setban("ASABC", "add")  # Non-numeric AS number
            assert False, "Should have failed to ban non-numeric AS"
        except Exception as e:
            assert "Invalid AS number" in str(e)

        # Test double-banning the same AS
        node.setban("AS5555", "add")
        try:
            node.setban("AS5555", "add")
            assert False, "Should have failed to double-ban AS"
        except Exception as e:
            assert "already banned" in str(e)

        # Test unbanning non-existent AS
        try:
            node.setban("AS6666", "remove")
            assert False, "Should have failed to unban non-existent AS"
        except Exception as e:
            assert "not previously manually banned" in str(e)

        # Clean up
        node.setban("AS5555", "remove")
        assert_equal(len(node.listbanned()), 0)

if __name__ == '__main__':
    SetBanTests(__file__).main()
