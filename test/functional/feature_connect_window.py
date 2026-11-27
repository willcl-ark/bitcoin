#!/usr/bin/env python3
# Copyright (c) 2026-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Validate blocks concurrently during IBD via ParallelIBDWindow.

Exercises multi-block parallel IBD validation across chained cross-block and
intra-block transactions, comparing the resulting chain tip and UTXO set hash
against serial validation (-prevoutfetchthreads=0), verifying -reindex-chainstate,
-reindex, exact -stopatheight stopping, and serial fallback + reorg recovery when
a fork contains an invalid missing-input block or a cross-block double-spend
conflict inside the window.
"""

from decimal import Decimal
import random
import time

from test_framework.blocktools import (
    COINBASE_MATURITY,
    add_witness_commitment,
    create_block,
    create_coinbase,
)
from test_framework.messages import (
    CBlock,
    CBlockHeader,
    COutPoint,
    CTransaction,
    CTxIn,
    CTxOut,
    from_hex,
    msg_headers,
)
from test_framework.p2p import P2PDataStore
from test_framework.script import CScript, OP_TRUE
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal
from test_framework.wallet import MiniWallet

NUM_TX_BLOCKS = 12
TXS_PER_BLOCK = 25
FORK_LENGTH = 6
INVALID_AT = 3  # 1-based position of invalid block in fork


class ConnectWindowTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 5
        self.extra_args = [
            [],
            ["-par=4", "-prevoutfetchthreads=4"],
            ["-par=4", "-prevoutfetchthreads=0"],
            ["-par=4", "-prevoutfetchthreads=4"],
            ["-par=4", "-prevoutfetchthreads=4"],
        ]

    def setup_network(self):
        self.setup_nodes()

    def build_source_chain(self):
        node = self.nodes[0]
        # Keep block timestamps 2 days in the past so syncing nodes remain in IBD.
        self.mocktime = int(time.time()) - 2 * 24 * 3600
        node.setmocktime(self.mocktime)
        self.wallet = MiniWallet(node)
        self.generate(self.wallet, 101, sync_fun=self.no_op)

        self.log.info(f"Mining {NUM_TX_BLOCKS} blocks of {TXS_PER_BLOCK} chained transactions each")
        utxo = self.wallet.get_utxo()
        for _ in range(NUM_TX_BLOCKS):
            chain = self.wallet.send_self_transfer_chain(
                from_node=node,
                chain_length=TXS_PER_BLOCK,
                utxo_to_spend=utxo,
            )
            utxo = chain[-1]["new_utxo"]
            self.generate(node, 1, sync_fun=self.no_op)
        assert_equal(node.getmempoolinfo()["size"], 0)

    def check_same_state(self, nodes):
        best = nodes[0].getbestblockhash()
        utxo = nodes[0].gettxoutsetinfo()["hash_serialized_3"]
        for node in nodes[1:]:
            assert_equal(node.getbestblockhash(), best)
            assert_equal(node.gettxoutsetinfo()["hash_serialized_3"], utxo)

    def test_sync_reindex_and_stopatheight(self):
        self.log.info("Compare parallel IBD sync (node 1) against serial baseline (node 2)")
        for i in (1, 2):
            self.connect_nodes(i, 0)
        self.sync_blocks(self.nodes[:3])
        self.check_same_state(self.nodes[:3])
        assert self.nodes[1].getblockchaininfo()["initialblockdownload"]

        self.stop_node(1)
        self.stop_node(2)

        self.log.info("Reindex chainstate with all blocks on disk (fills parallel IBD window)")
        self.start_node(1, extra_args=self.extra_args[1] + ["-reindex-chainstate"])
        self.wait_until(lambda: self.nodes[1].getblockcount() == self.nodes[0].getblockcount(), timeout=120)
        self.check_same_state([self.nodes[0], self.nodes[1]])
        self.stop_node(1)

        self.log.info("Verify -stopatheight stops at exact target height during parallel -reindex-chainstate")
        stop_height = self.nodes[0].getblockcount() - 5
        log_pos = self.nodes[1].debug_log_size()
        self.nodes[1].start(extra_args=self.extra_args[1] + ["-reindex-chainstate", f"-stopatheight={stop_height}"])
        self.nodes[1].wait_until_stopped(timeout=120)
        with open(self.nodes[1].debug_log_path, encoding="utf-8") as log:
            log.seek(log_pos)
            tips = [line for line in log if "UpdateTip:" in line]
        assert tips and f"height={stop_height} " in tips[-1], f"Expected final height={stop_height}, got: {tips[-1] if tips else None}"

        self.log.info("Reindex blocks and chainstate through the parallel window")
        self.start_node(1, extra_args=self.extra_args[1] + ["-reindex"])
        self.wait_until(lambda: self.nodes[1].getblockcount() == self.nodes[0].getblockcount(), timeout=120)
        self.check_same_state([self.nodes[0], self.nodes[1]])
        assert self.nodes[1].verifychain(checklevel=4, nblocks=0)
        self.stop_node(1)

    def test_invalid_block_in_window(self, node_idx, *, conflict):
        node0, target = self.nodes[0], self.nodes[node_idx]
        height = node0.getblockcount()
        kind = "cross-block double-spend conflict" if conflict else "missing input"
        self.log.info(f"Serve node {node_idx} a longer fork containing an invalid block ({kind})")

        main_blocks = [from_hex(CBlock(), node0.getblock(node0.getblockhash(h), 0)) for h in range(1, height + 1)]
        fork_base_height = height - FORK_LENGTH // 2
        fork_base = main_blocks[fork_base_height - 1]
        fork_blocks = []
        if conflict:
            utxo = self.wallet.get_utxo()
            assert utxo["height"] + COINBASE_MATURITY <= fork_base_height + 1
            first_spend = self.wallet.create_self_transfer(utxo_to_spend=utxo)["tx"]
            second_spend = self.wallet.create_self_transfer(utxo_to_spend=utxo, fee_rate=Decimal("0.005"))["tx"]
            assert first_spend.txid_hex != second_spend.txid_hex

        prev_hash, prev_time = fork_base.hash_int, fork_base.nTime
        for n in range(1, FORK_LENGTH + 1):
            h = fork_base_height + n
            txlist = []
            if conflict and n == INVALID_AT - 1:
                txlist.append(first_spend)
            elif conflict and n == INVALID_AT:
                txlist.append(second_spend)
            elif n == INVALID_AT:
                bogus = CTransaction()
                bogus.vin.append(CTxIn(COutPoint(random.getrandbits(256), 0), b""))
                bogus.vout.append(CTxOut(1, CScript([OP_TRUE])))
                txlist.append(bogus)
            block = create_block(prev_hash, create_coinbase(h), ntime=prev_time + 1, txlist=txlist)
            if conflict and txlist:
                add_witness_commitment(block)
            block.solve()
            fork_blocks.append(block)
            prev_hash, prev_time = block.hash_int, block.nTime

        peer = target.add_p2p_connection(P2PDataStore())
        for block in main_blocks + fork_blocks:
            peer.block_store[block.hash_int] = block
        peer.last_block_hash = fork_blocks[-1].hash_int

        with target.assert_debug_log(["bad-txns-inputs-missingorspent"]):
            peer.send_without_ping(msg_headers(headers=[CBlockHeader(b) for b in main_blocks]))
            peer.send_without_ping(msg_headers(headers=[CBlockHeader(b) for b in fork_blocks]))
            self.wait_until(lambda: any(t["status"] == "invalid" for t in target.getchaintips()), timeout=120)

        tips = {t["hash"]: t["status"] for t in target.getchaintips()}
        assert_equal(tips[fork_blocks[-1].hash_hex], "invalid")
        assert_equal(target.getbestblockhash(), fork_blocks[INVALID_AT - 2].hash_hex)
        assert_equal(target.getblockcount(), fork_base_height + INVALID_AT - 1)
        target.disconnect_p2ps()

        self.log.info(f"Node {node_idx} reorgs cleanly onto the valid main chain")
        self.connect_nodes(node_idx, 0)
        self.generate(node0, 3, sync_fun=self.no_op)
        self.sync_blocks([node0, target])
        self.check_same_state([node0, target])
        assert target.verifychain(checklevel=4, nblocks=0)

    def run_test(self):
        self.build_source_chain()
        self.test_sync_reindex_and_stopatheight()
        self.test_invalid_block_in_window(3, conflict=False)
        self.test_invalid_block_in_window(4, conflict=True)


if __name__ == "__main__":
    ConnectWindowTest(__file__).main()
