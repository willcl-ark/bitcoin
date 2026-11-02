#!/usr/bin/env python3
#
# Copyright (c) 2026-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit.

"""Check existing developer documentation for Doxygen warnings."""

from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    root = Path(__file__).resolve().parents[2]
    with tempfile.TemporaryDirectory(prefix="bitcoin-doxygen-") as tmp:
        config = (root / "doc/Doxyfile.in").read_text(encoding="utf-8")
        config = config.replace("@CLIENT_VERSION_STRING@", "lint")
        config = config.replace("@PROJECT_BINARY_DIR@", tmp)
        config += f'''
OUTPUT_DIRECTORY = "{tmp}/doxygen"
QUIET = YES
HAVE_DOT = NO
WARN_AS_ERROR = FAIL_ON_WARNINGS
'''
        doxyfile = Path(tmp) / "Doxyfile"
        doxyfile.write_text(config, encoding="utf-8")
        try:
            return subprocess.run(["doxygen", str(doxyfile)], cwd=root).returncode
        except FileNotFoundError:
            print("Doxygen is required. Install it or run ./ci/lint.py.", file=sys.stderr)
            return 1


if __name__ == "__main__":
    sys.exit(main())
