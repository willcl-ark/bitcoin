#!/usr/bin/env python3
# Copyright (c) 2025-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

"""
List functional tests for CMake test discovery.
"""

import sys
import os

# Add the current directory to sys.path so we can import test_runner
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

try:
    import test_runner
    # Output all tests (BASE_SCRIPTS + EXTENDED_SCRIPTS)
    # all_tests = test_runner.BASE_SCRIPTS + test_runner.EXTENDED_SCRIPTS
    all_tests = test_runner.BASE_SCRIPTS
    for test in all_tests:
        print(test)
except ImportError as e:
    print(f"Error importing test_runner: {e}", file=sys.stderr)
    sys.exit(1)
except Exception as e:
    print(f"Error listing tests: {e}", file=sys.stderr)
    sys.exit(1)
