#!/usr/bin/env python3
# Copyright (c) The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test scripthash index RPCs."""

import hashlib

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error
from test_framework.wallet import MiniWallet, getnewdestination


def scripthash(script_pub_key):
    return hashlib.sha256(bytes(script_pub_key)).hexdigest()


class ScriptHashIndexTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 3
        self.extra_args = [
            [],
            ["-scripthashindex"],
            ["-scripthashindex", "-txospenderindex"],
        ]

    def wait_for_index(self, node, height):
        self.wait_until(lambda: node.getindexinfo()["scripthashindex"]["synced"])
        self.wait_until(lambda: node.getindexinfo()["scripthashindex"]["best_block_height"] >= height)

    def wait_for_shared_indexes(self, node, height):
        self.wait_for_index(node, height)
        self.wait_until(lambda: node.getindexinfo()["txospenderindex"]["synced"])
        self.wait_until(lambda: node.getindexinfo()["txospenderindex"]["best_block_height"] >= height)

    def run_test(self):
        no_index_node = self.nodes[0]
        node = self.nodes[1]
        shared_node = self.nodes[2]
        wallet = MiniWallet(node)

        zero_scripthash = "00" * 32
        assert_raises_rpc_error(-1, "scripthash index is not enabled", no_index_node.getscripthashactivity, zero_scripthash)
        assert_raises_rpc_error(-8, "scripthash must be of length 64", node.getscripthashactivity, "00")
        assert_raises_rpc_error(-8, "scripthash must be hexadecimal string", node.getscripthashactivity, "zz" * 32)
        self.stop_node(0)
        self.nodes[0].assert_start_raises_init_error(
            expected_msg="Error: Prune mode is incompatible with -scripthashindex.",
            extra_args=["-prune=550", "-scripthashindex"],
        )
        self.start_node(0)
        self.connect_nodes(0, 1)
        self.connect_nodes(1, 2)

        self.generate(wallet, 101)
        self.wait_for_index(node, node.getblockcount())
        self.wait_for_shared_indexes(shared_node, node.getblockcount())

        assert_equal(node.getscripthashactivity(zero_scripthash), {
            "history": [],
            "utxos": [],
            "balance": 0,
            "bestblock": node.getbestblockhash(),
            "height": node.getblockcount(),
        })
        assert_equal(shared_node.getscripthashactivity(zero_scripthash), {
            "history": [],
            "utxos": [],
            "balance": 0,
            "bestblock": shared_node.getbestblockhash(),
            "height": shared_node.getblockcount(),
        })

        _, script_pub_key, _ = getnewdestination()
        amount = 123456789
        tx = wallet.send_to(from_node=node, scriptPubKey=script_pub_key, amount=amount)
        block_hash = self.generate(node, 1)[0]
        block_height = node.getblockheader(block_hash)["height"]
        self.wait_for_index(node, block_height)
        self.wait_for_shared_indexes(shared_node, block_height)

        activity = node.getscripthashactivity(scripthash(script_pub_key))
        assert_equal(activity["height"], block_height)
        assert_equal(activity["bestblock"], block_hash)
        assert_equal(activity["balance"], amount)
        assert_equal(activity["history"], [{"txid": tx["txid"], "height": block_height}])
        assert_equal(activity["utxos"], [{
            "txid": tx["txid"],
            "vout": tx["sent_vout"],
            "height": block_height,
            "value": amount,
        }])
        assert_equal(shared_node.getscripthashactivity(scripthash(script_pub_key)), activity)

        wallet_script_pub_key = wallet.get_output_script()
        wallet_scripthash = scripthash(wallet_script_pub_key)
        receive_tx = wallet.send_to(from_node=node, scriptPubKey=wallet_script_pub_key, amount=amount)
        receive_block = self.generate(node, 1)[0]
        receive_height = node.getblockheader(receive_block)["height"]
        wallet.rescan_utxos()
        self.wait_for_index(node, receive_height)
        self.wait_for_shared_indexes(shared_node, receive_height)

        received_activity = node.getscripthashactivity(wallet_scripthash)
        assert_equal(received_activity["history"][-1], {"txid": receive_tx["txid"], "height": receive_height})
        received_utxo = {
            "txid": receive_tx["txid"],
            "vout": receive_tx["sent_vout"],
            "height": receive_height,
            "value": amount,
        }
        assert received_utxo in received_activity["utxos"]
        assert_equal(shared_node.getscripthashactivity(wallet_scripthash), received_activity)

        spend_tx = wallet.send_self_transfer(
            from_node=node,
            utxo_to_spend=wallet.get_utxo(txid=receive_tx["txid"], vout=receive_tx["sent_vout"]),
        )
        spend_block = self.generate(node, 1)[0]
        spend_height = node.getblockheader(spend_block)["height"]
        self.wait_for_index(node, spend_height)
        self.wait_for_shared_indexes(shared_node, spend_height)

        spent_activity = node.getscripthashactivity(wallet_scripthash)
        assert {"txid": spend_tx["txid"], "height": spend_height} in spent_activity["history"]
        assert received_utxo not in spent_activity["utxos"]
        assert_equal(shared_node.getscripthashactivity(wallet_scripthash), spent_activity)

        node.invalidateblock(spend_block)
        assert_raises_rpc_error(-1, "scripthash index is catching up", node.getscripthashactivity, wallet_scripthash)

        reorg_block = node.generateblock(output=node.get_deterministic_priv_key().address, transactions=[], called_by_framework=True)["hash"]
        self.wait_until(lambda: node.getscripthashactivity(wallet_scripthash)["bestblock"] == reorg_block)

        reorged_activity = node.getscripthashactivity(wallet_scripthash)
        assert {"txid": spend_tx["txid"], "height": spend_height} not in reorged_activity["history"]
        assert received_utxo in reorged_activity["utxos"]

        assert_equal(node.getscripthashhistory(scripthash(script_pub_key)), activity["history"])
        assert_equal(node.getscripthashutxos(scripthash(script_pub_key)), activity["utxos"])
        assert_equal(node.getscripthashbalance(scripthash(script_pub_key)), {"confirmed": amount})


if __name__ == "__main__":
    ScriptHashIndexTest(__file__).main()
