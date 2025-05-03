#!/usr/bin/env python3
# Copyright (c) The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Corrupt block-file metadata must not cause block or undo data loss."""

import hashlib

from test_framework.test_framework import BitcoinTestFramework
from test_framework.test_node import ErrorMatch
from test_framework.util import assert_equal


class BlockTreeCorruptionTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.uses_wallet = False
        self.extra_args = [[], ["-prune=1", "-fastprune"]]

    def setup_network(self):
        self.setup_nodes()

    def run_test(self):
        for node in self.nodes:
            self.generate(node, 600, sync_fun=self.no_op)
            tip = node.getbestblockhash()
            if node.index == 1:
                node.pruneblockchain(300)
            self.stop_node(node.index)

            blocks_dir = node.chain_path / "blocks"
            metadata_path = blocks_dir / "index" / "blockfiles.dat"
            metadata = metadata_path.read_bytes()
            data_paths = sorted(blocks_dir.glob("blk*.dat")) + sorted(blocks_dir.glob("rev*.dat"))
            data_hashes = {path: hashlib.sha256(path.read_bytes()).digest() for path in data_paths}
            block_files = sorted(blocks_dir.glob("blk*.dat"))
            # Test the active file's append offset, and an older retained file
            # that startup must not mistake for an already pruned file.
            files_to_corrupt = [block_files[-1]]
            if node.index == 1:
                assert len(block_files) > 1
                files_to_corrupt.append(block_files[0])

            for block_file in files_to_corrupt:
                self.log.info(f"Corrupting metadata for {block_file.name}")
                file_index = int(block_file.stem[3:])
                # The format has an 8-byte file header and 40-byte records,
                # consisting of 36 bytes of metadata followed by a CRC32C.
                checksum_offset = 8 + file_index * 40 + 36
                corrupted = bytearray(metadata)
                corrupted[checksum_offset] ^= 1
                metadata_path.write_bytes(corrupted)

                with node.assert_debug_log(["Record data failed integrity check"]):
                    node.assert_start_raises_init_error(expected_msg="Error loading databases", match=ErrorMatch.PARTIAL_REGEX)
                assert_equal(sorted(blocks_dir.glob("blk*.dat")) + sorted(blocks_dir.glob("rev*.dat")), data_paths)
                for path, expected_hash in data_hashes.items():
                    assert_equal(hashlib.sha256(path.read_bytes()).digest(), expected_hash)
                metadata_path.write_bytes(metadata)

            truncations = {"all records": metadata[:8]}
            if len(metadata) > 48:
                truncations["last record"] = metadata[:-40]
            for description, truncated in truncations.items():
                self.log.info(f"Removing {description} from block-file metadata")
                metadata_path.write_bytes(truncated)
                with node.assert_debug_log(["Missing block file metadata"]):
                    node.assert_start_raises_init_error(expected_msg="Error loading block database", match=ErrorMatch.PARTIAL_REGEX)
                assert_equal(sorted(blocks_dir.glob("blk*.dat")) + sorted(blocks_dir.glob("rev*.dat")), data_paths)
                for path, expected_hash in data_hashes.items():
                    assert_equal(hashlib.sha256(path.read_bytes()).digest(), expected_hash)
                metadata_path.write_bytes(metadata)

            self.start_node(node.index)
            assert_equal(node.getbestblockhash(), tip)
            self.generate(node, 1, sync_fun=self.no_op)
            assert_equal(node.getblock(tip)["height"], 600)
            self.stop_node(node.index)


if __name__ == "__main__":
    BlockTreeCorruptionTest(__file__).main()
