#!/usr/bin/env python3
# Copyright (c) 2014-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the -alertnotify, -blocknotify and -walletnotify options."""
import os
import platform
import shlex
import sys

from test_framework.address import ADDRESS_BCRT1_UNSPENDABLE
from test_framework.blocktools import (
    create_block,
)
from test_framework.descriptors import descsum_create
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    ensure_for,
)

# Linux allow all characters other than \x00
# Windows disallow control characters (0-31) and /\?%:|"<>
FILE_CHAR_START = 32 if platform.system() == 'Windows' else 1
FILE_CHAR_END = 128
FILE_CHARS_DISALLOWED = '/\\?%*:|"<>' if platform.system() == 'Windows' else '/'
UNCONFIRMED_HASH_STRING = 'unconfirmed'

LARGE_WORK_INVALID_CHAIN_WARNING = (
    "Warning: Found invalid chain more than 6 blocks longer than our best chain. This could be due to database corruption or consensus incompatibility with peers."
)


def notify_outputname(walletname, txid):
    return txid if platform.system() == 'Windows' else f'{walletname}_{txid}'


def shell_quote(s):
    return f'"{s}"' if platform.system() == 'Windows' else shlex.quote(s)


class NotificationsTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 3
        self.setup_clean_chain = True
        self.uses_wallet = None

    def setup_network(self):
        self.wallet = ''.join(chr(i) for i in range(FILE_CHAR_START, FILE_CHAR_END) if chr(i) not in FILE_CHARS_DISALLOWED)
        self.alertnotify_dir = os.path.join(self.options.tmpdir, "alertnotify")
        self.alertnotify_file = os.path.join(self.alertnotify_dir, "alertnotify.txt")
        self.blocknotify_dir = os.path.join(self.options.tmpdir, "blocknotify")
        self.blocknotify_ignored_file = os.path.join(self.blocknotify_dir, "ignored.txt")
        self.reindex_blocknotify_dir = os.path.join(self.options.tmpdir, "reindex_blocknotify")
        self.walletnotify_dir = os.path.join(self.options.tmpdir, "walletnotify")
        self.shutdownnotify_dir = os.path.join(self.options.tmpdir, "shutdownnotify")
        self.shutdownnotify_file = os.path.join(self.shutdownnotify_dir, "shutdownnotify.txt")
        self.shutdownnotify_file_2 = os.path.join(self.shutdownnotify_dir, "shutdownnotify_2.txt")
        self.shutdownnotify_started_file = os.path.join(self.shutdownnotify_dir, "shutdownnotify_started.txt")
        self.shutdownnotify_wait_file = os.path.join(self.shutdownnotify_dir, "shutdownnotify_wait")
        self.shutdownnotify_script = os.path.join(self.shutdownnotify_dir, "shutdownnotify.py")
        os.mkdir(self.alertnotify_dir)
        os.mkdir(self.blocknotify_dir)
        os.mkdir(self.reindex_blocknotify_dir)
        os.mkdir(self.walletnotify_dir)
        os.mkdir(self.shutdownnotify_dir)
        with open(self.shutdownnotify_script, "w", encoding="utf8") as f:
            f.write(
                "import pathlib\n"
                "import sys\n"
                "import time\n"
                "\n"
                "wait_file = pathlib.Path(sys.argv[1])\n"
                "started_file = pathlib.Path(sys.argv[2])\n"
                "done_file = pathlib.Path(sys.argv[3])\n"
                "started_file.write_text('started\\n', encoding='utf8')\n"
                "for _ in range(1000):\n"
                "    if wait_file.exists():\n"
                "        done_file.write_text('done\\n', encoding='utf8')\n"
                "        sys.exit(0)\n"
                "    time.sleep(0.05)\n"
                "raise SystemExit('timed out waiting for ' + str(wait_file))\n"
            )
        # -alertnotify and -blocknotify on node0, walletnotify on node1,
        # an isolated -blocknotify on node2 used to test reindex suppression.
        self.extra_args = [[
            f"-alertnotify=echo %s >> {self.alertnotify_file}",
            f"-blocknotify=echo > {self.blocknotify_ignored_file}",
            f"-blocknotify=echo > {os.path.join(self.blocknotify_dir, '%s')}",
            f"-shutdownnotify=echo > {self.shutdownnotify_file}",
        ], [
            f"-walletnotify=echo %h_%b > {os.path.join(self.walletnotify_dir, notify_outputname('%w', '%s'))}",
        ], [
            f"-blocknotify=echo > {os.path.join(self.reindex_blocknotify_dir, '%s')}",
        ]]
        # In Windows cross-build CI, bitcoind is a Windows process and cannot
        # reliably launch the test runner's Python helper through
        # -shutdownnotify.
        if platform.system() != 'Windows':
            wait_shutdownnotify_cmd = " ".join(shell_quote(s) for s in [
                sys.executable,
                self.shutdownnotify_script,
                self.shutdownnotify_wait_file,
                self.shutdownnotify_started_file,
                self.shutdownnotify_file_2,
            ])
            self.extra_args[0].append(f"-shutdownnotify={wait_shutdownnotify_cmd}")
        self.wallet_names = [self.default_wallet_name, self.wallet]
        super().setup_network()

    def run_test(self):
        if self.is_wallet_compiled():
            # Setup the descriptors to be imported to the wallet
            xpriv = "tprv8ZgxMBicQKsPfHCsTwkiM1KT56RXbGGTqvc2hgqzycpwbHqqpcajQeMRZoBD35kW4RtyCemu6j34Ku5DEspmgjKdt2qe4SvRch5Kk8B8A2v"
            desc_imports = [{
                "desc": descsum_create(f"wpkh({xpriv}/0/*)"),
                "timestamp": 0,
                "active": True,
                "keypool": True,
            },{
                "desc": descsum_create(f"wpkh({xpriv}/1/*)"),
                "timestamp": 0,
                "active": True,
                "keypool": True,
                "internal": True,
            }]
            # Make the wallets and import the descriptors
            # Ensures that node 0 and node 1 share the same wallet for the conflicting transaction tests below.
            for i, name in enumerate(self.wallet_names):
                self.nodes[i].createwallet(wallet_name=name, blank=True, load_on_startup=True)
                self.nodes[i].importdescriptors(desc_imports)

        self.log.info("test -blocknotify")
        block_count = 10
        blocks = self.generatetoaddress(self.nodes[1], block_count, self.nodes[1].getnewaddress() if self.is_wallet_compiled() else ADDRESS_BCRT1_UNSPENDABLE)

        # wait at most 10 seconds for expected number of files before reading the content
        self.wait_until(lambda: len(os.listdir(self.blocknotify_dir)) == block_count, timeout=10)

        # directory content should equal the generated blocks hashes
        assert_equal(sorted(blocks), sorted(os.listdir(self.blocknotify_dir)))
        assert_equal(os.path.exists(self.blocknotify_ignored_file), False)

        self.log.info("test -blocknotify after best block changes")
        self.wait_until(self.remove_blocknotify_files, timeout=10)

        tip = self.nodes[0].getbestblockhash()
        previous_tip = self.nodes[0].getblock(tip)["previousblockhash"]

        self.nodes[0].invalidateblock(tip)
        self.wait_until(lambda: previous_tip in os.listdir(self.blocknotify_dir), timeout=10)

        self.nodes[0].reconsiderblock(tip)
        self.wait_until(lambda: tip in os.listdir(self.blocknotify_dir), timeout=10)

        self.log.info("test -blocknotify is suppressed during reindex")
        # Node 2 received the same blocks via P2P; drain those notifications
        # before restarting it so any post-restart entries are fresh.
        expected_tip = self.nodes[2].getbestblockhash()
        self.wait_until(lambda: expected_tip in os.listdir(self.reindex_blocknotify_dir), timeout=10)
        for f in os.listdir(self.reindex_blocknotify_dir):
            os.remove(os.path.join(self.reindex_blocknotify_dir, f))

        self.disconnect_nodes(1, 2)
        self.restart_node(2, extra_args=self.extra_args[2] + ["-reindex"])
        # Wait for reindex to reach POST_INIT (i.e. leave INIT_REINDEX); only
        # then would -blocknotify fire if it were going to.
        self.wait_until(
            lambda: self.nodes[2].getbestblockhash() == expected_tip
                and not self.nodes[2].getblockchaininfo()["initialblockdownload"],
            timeout=60,
        )
        ensure_for(duration=1, f=lambda: not os.listdir(self.reindex_blocknotify_dir))

        # A new block after reindex completes still triggers -blocknotify.
        post_reindex_tip = self.generatetoaddress(self.nodes[2], 1, ADDRESS_BCRT1_UNSPENDABLE, sync_fun=self.no_op)[0]
        self.wait_until(lambda: post_reindex_tip in os.listdir(self.reindex_blocknotify_dir), timeout=10)
        assert_equal(os.listdir(self.reindex_blocknotify_dir), [post_reindex_tip])

        self.connect_nodes(1, 2)

        if self.is_wallet_compiled():
            self.log.info("test -walletnotify")
            # wait at most 10 seconds for expected number of files before reading the content
            self.wait_until(lambda: len(os.listdir(self.walletnotify_dir)) == block_count, timeout=10)

            # directory content should equal the generated transaction hashes
            tx_details = list(map(lambda t: (t['txid'], t['blockheight'], t['blockhash']), self.nodes[1].listtransactions("*", block_count)))
            self.expect_wallet_notify(tx_details)

            self.log.info("test -walletnotify after rescan")
            # rescan to force wallet notifications
            self.nodes[1].rescanblockchain()
            self.wait_until(lambda: len(os.listdir(self.walletnotify_dir)) == block_count, timeout=10)

            self.connect_nodes(0, 1)

            # directory content should equal the generated transaction hashes
            tx_details = list(map(lambda t: (t['txid'], t['blockheight'], t['blockhash']), self.nodes[1].listtransactions("*", block_count)))
            self.expect_wallet_notify(tx_details)

            # Conflicting transactions tests.
            # Generate spends from node 0, and check notifications
            # triggered by node 1
            self.log.info("test -walletnotify with conflicting transactions")
            self.nodes[0].rescanblockchain()
            self.generatetoaddress(self.nodes[0], 100, ADDRESS_BCRT1_UNSPENDABLE)

            # Generate transaction on node 0, sync mempools, and check for
            # notification on node 1.
            tx1 = self.nodes[0].sendtoaddress(address=ADDRESS_BCRT1_UNSPENDABLE, amount=1, replaceable=True)
            assert_equal(tx1 in self.nodes[0].getrawmempool(), True)
            self.sync_mempools()
            self.expect_wallet_notify([(tx1, -1, UNCONFIRMED_HASH_STRING)])

            # Generate bump transaction, sync mempools, and check for bump1
            # notification. In the future, per
            # https://github.com/bitcoin/bitcoin/pull/9371, it might be better
            # to have notifications for both tx1 and bump1.
            bump1 = self.nodes[0].bumpfee(tx1)["txid"]
            assert_equal(bump1 in self.nodes[0].getrawmempool(), True)
            self.sync_mempools()
            self.expect_wallet_notify([(bump1, -1, UNCONFIRMED_HASH_STRING)])

            # Add bump1 transaction to new block, checking for a notification
            # and the correct number of confirmations.
            blockhash1 = self.generatetoaddress(self.nodes[0], 1, ADDRESS_BCRT1_UNSPENDABLE)[0]
            blockheight1 = self.nodes[0].getblockcount()
            self.sync_blocks()
            self.expect_wallet_notify([(bump1, blockheight1, blockhash1)])
            assert_equal(self.nodes[1].gettransaction(bump1)["confirmations"], 1)

            # Generate a second transaction to be bumped.
            tx2 = self.nodes[0].sendtoaddress(address=ADDRESS_BCRT1_UNSPENDABLE, amount=1, replaceable=True)
            assert_equal(tx2 in self.nodes[0].getrawmempool(), True)
            self.sync_mempools()
            self.expect_wallet_notify([(tx2, -1, UNCONFIRMED_HASH_STRING)])

            # Bump tx2 as bump2 and generate a block on node 0 while
            # disconnected, then reconnect and check for notifications on node 1
            # about newly confirmed bump2 and newly conflicted tx2.
            self.disconnect_nodes(0, 1)
            bump2 = self.nodes[0].bumpfee(tx2)["txid"]
            blockhash2 = self.generatetoaddress(self.nodes[0], 1, ADDRESS_BCRT1_UNSPENDABLE, sync_fun=self.no_op)[0]
            blockheight2 = self.nodes[0].getblockcount()
            assert_equal(self.nodes[0].gettransaction(bump2)["confirmations"], 1)
            assert_equal(tx2 in self.nodes[1].getrawmempool(), True)
            self.connect_nodes(0, 1)
            self.sync_blocks()
            self.expect_wallet_notify([(bump2, blockheight2, blockhash2), (tx2, -1, UNCONFIRMED_HASH_STRING)])
            assert_equal(self.nodes[1].gettransaction(bump2)["confirmations"], 1)

        self.log.info("test -alertnotify with large work invalid chain")
        # create a bunch of invalid blocks
        tip = self.nodes[0].getbestblockhash()
        height = self.nodes[0].getblockcount() + 1
        block_time = self.nodes[0].getblock(tip)['time'] + 1

        invalid_blocks = []
        for _ in range(7):  # invalid chain must be longer than 6 blocks to trigger warning
            block = create_block(int(tip, 16), height=height, ntime=block_time)
            # make block invalid by exceeding block subsidy
            block.vtx[0].vout[0].nValue += 1
            block.hashMerkleRoot = block.calc_merkle_root()
            block.solve()
            invalid_blocks.append(block)
            tip = block.hash_hex
            height += 1
            block_time += 1

        # submit headers of invalid blocks
        for invalid_block in invalid_blocks:
            self.nodes[0].submitheader(invalid_block.serialize().hex())
        # submit invalid blocks in reverse order (tip first, to set m_best_invalid)
        for invalid_block in reversed(invalid_blocks):
            self.nodes[0].submitblock(invalid_block.serialize().hex())

        self.wait_until(lambda: os.path.isfile(self.alertnotify_file), timeout=10)
        self.wait_until(self.large_work_invalid_chain_warning_in_alert_file, timeout=10)

        self.log.info("test -shutdownnotify")
        for node in self.nodes:
            node.stop_node(wait_until_stopped=False)
        self.wait_until(lambda: os.path.isfile(self.shutdownnotify_file), timeout=10)
        if platform.system() != 'Windows':
            try:
                self.wait_until(lambda: os.path.isfile(self.shutdownnotify_started_file), timeout=10)
                ensure_for(duration=1, f=lambda: self.nodes[0].process.poll() is None)
            finally:
                with open(self.shutdownnotify_wait_file, "w", encoding="utf8"):
                    pass
            self.wait_until(lambda: os.path.isfile(self.shutdownnotify_file_2), timeout=10)
        for node in self.nodes:
            node.wait_until_stopped()

    def large_work_invalid_chain_warning_in_alert_file(self):
        with open(self.alertnotify_file, 'r') as f:
            alert_text = f.read()
        return LARGE_WORK_INVALID_CHAIN_WARNING in alert_text

    def remove_blocknotify_files(self):
        for block_file in os.listdir(self.blocknotify_dir):
            try:
                os.remove(os.path.join(self.blocknotify_dir, block_file))
            except OSError:
                return False
        return not os.listdir(self.blocknotify_dir)

    def expect_wallet_notify(self, tx_details):
        self.wait_until(lambda: len(os.listdir(self.walletnotify_dir)) >= len(tx_details), timeout=10)
        # Should have no more and no less files than expected
        assert_equal(sorted(notify_outputname(self.wallet, tx_id) for tx_id, _, _ in tx_details), sorted(os.listdir(self.walletnotify_dir)))
        # Should now verify contents of each file
        for tx_id, blockheight, blockhash in tx_details:
            fname = os.path.join(self.walletnotify_dir, notify_outputname(self.wallet, tx_id))
            # Wait for the cached writes to hit storage
            self.wait_until(lambda: os.path.getsize(fname) > 0, timeout=10)
            with open(fname, 'rt') as f:
                text = f.read()
                # Universal newline ensures '\n' on 'nt'
                assert_equal(text[-1], '\n')
                text = text[:-1]
                if platform.system() == 'Windows':
                    # On Windows, echo as above will append a whitespace
                    assert_equal(text[-1], ' ')
                    text = text[:-1]
                expected = str(blockheight) + '_' + blockhash
                assert_equal(text, expected)

        for tx_file in os.listdir(self.walletnotify_dir):
            os.remove(os.path.join(self.walletnotify_dir, tx_file))


if __name__ == '__main__':
    NotificationsTest(__file__).main()
