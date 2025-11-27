#!/usr/bin/env python3
# Copyright (c) 2025 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Generate a basic coverage report for the RPC interface.

This script aggregates RPC coverage data from functional tests and reports
which RPC commands were not exercised during testing.
"""

import argparse
import os
import sys


def get_uncovered_rpc_commands(coverage_dir):
    """
    Return a set of currently untested RPC commands.

    This logic is extracted from test_runner.py's RPCCoverage class.
    """
    reference_filename = "rpc_interface.txt"
    coverage_file_prefix = "coverage."

    coverage_ref_filename = os.path.join(coverage_dir, reference_filename)
    coverage_filenames = set()
    all_cmds = set()
    # Consider RPC generate covered, because it is overloaded in
    # test_framework/test_node.py and not seen by the coverage check.
    covered_cmds = set({"generate"})

    if not os.path.isfile(coverage_ref_filename):
        raise RuntimeError(f"No coverage reference found at {coverage_ref_filename}")

    with open(coverage_ref_filename, "r", encoding="utf8") as coverage_ref_file:
        all_cmds.update([line.strip() for line in coverage_ref_file.readlines()])

    for root, _, files in os.walk(coverage_dir):
        for filename in files:
            if filename.startswith(coverage_file_prefix):
                coverage_filenames.add(os.path.join(root, filename))

    for filename in coverage_filenames:
        with open(filename, "r", encoding="utf8") as coverage_file:
            covered_cmds.update([line.strip() for line in coverage_file.readlines()])

    return all_cmds - covered_cmds


def report_rpc_coverage(coverage_dir):
    """
    Print out RPC commands that were unexercised by tests.
    Returns True if all commands are covered, False otherwise.
    """
    try:
        uncovered = get_uncovered_rpc_commands(coverage_dir)
    except RuntimeError as e:
        print(f"Error: {e}")
        return False

    if uncovered:
        print("Uncovered RPC commands:")
        for command in sorted(uncovered):
            print(f"  - {command}")
        return False
    else:
        print("All RPC commands covered.")
        return True


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawTextHelpFormatter
    )
    parser.add_argument("coverage_dir", help="Directory containing RPC coverage data")

    args = parser.parse_args()

    if not os.path.isdir(args.coverage_dir):
        print(f"Error: Coverage directory '{args.coverage_dir}' does not exist")
        return 1

    coverage_passed = report_rpc_coverage(args.coverage_dir)
    return 0 if coverage_passed else 1


if __name__ == "__main__":
    sys.exit(main())

