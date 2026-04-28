#!/usr/bin/env python3
# Copyright (c) 2026-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Reset the shared functional test RPC coverage directory."""

import argparse
import os
import shutil


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--coveragedir", required=True)
    args = parser.parse_args()

    if os.path.isdir(args.coveragedir):
        shutil.rmtree(args.coveragedir)
    os.makedirs(args.coveragedir)


if __name__ == '__main__':
    main()
