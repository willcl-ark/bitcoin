#!/usr/bin/env python3
#
# Copyright (c) The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

"""
Enforce monotonic Python formatting: files that are already ruff-formatted
on the base branch must stay formatted. This allows incremental adoption
without a single big-bang reformat.
"""

import os
import shutil
import subprocess
import sys

SUBTREES = [
    "src/crc32c",
    "src/crypto/ctaes",
    "src/ipc/libmultiprocess",
    "src/leveldb",
    "src/minisketch",
    "src/secp256k1",
]


def get_base_commit():
    commit_range = os.environ.get("COMMIT_RANGE", "")
    if ".." in commit_range:
        return commit_range.split("..")[0]
    return None


def get_changed_py_files(base):
    result = subprocess.run(
        ["git", "diff", "--name-only", "--diff-filter=d", base, "HEAD", "--", "*.py"],
        capture_output=True,
        text=True,
        check=True,
    )
    files = [f for f in result.stdout.strip().splitlines() if f]
    return [f for f in files if not any(f.startswith(s + "/") for s in SUBTREES)]


def is_formatted_at_commit(commit, path):
    show = subprocess.run(
        ["git", "show", f"{commit}:{path}"],
        capture_output=True,
    )
    if show.returncode != 0:
        return False
    result = subprocess.run(
        ["ruff", "format", "--check", "--stdin-filename", path],
        input=show.stdout,
        capture_output=True,
    )
    return result.returncode == 0


def is_formatted_in_worktree(path):
    result = subprocess.run(
        ["ruff", "format", "--check", path],
        capture_output=True,
    )
    return result.returncode == 0


def main():
    if not shutil.which("ruff"):
        print("Skipping format check since ruff is not installed.")
        return

    base = get_base_commit()
    if base is None:
        print("Skipping format check: COMMIT_RANGE not set.")
        return

    changed_files = get_changed_py_files(base)
    if not changed_files:
        return

    failures = []
    for path in changed_files:
        if not is_formatted_at_commit(base, path):
            continue
        if not is_formatted_in_worktree(path):
            failures.append(path)

    if failures:
        print(
            "These files were ruff-formatted on the base branch but are no longer formatted:"
        )
        for f in failures:
            print(f"  {f}")
        print("\nRun 'ruff format' on these files to fix.")
        sys.exit(1)


if __name__ == "__main__":
    main()
