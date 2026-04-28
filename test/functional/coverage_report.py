#!/usr/bin/env python3
# Copyright (c) 2026-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Report functional test RPC coverage."""

import argparse
import sys

from test_framework.coverage import report_rpc_coverage


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--coveragedir", required=True)
    args = parser.parse_args()

    if not report_rpc_coverage(args.coveragedir):
        sys.exit(1)


if __name__ == '__main__':
    main()
