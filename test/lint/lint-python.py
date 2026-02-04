#!/usr/bin/env python3
#
# Copyright (c) 2022-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

"""
Check for type errors in python files.
"""

import os
from pathlib import Path
import shutil
import subprocess

from importlib.metadata import metadata, PackageNotFoundError

# Customize mypy cache dir via environment variable
cache_dir = Path(__file__).parent.parent / ".mypy_cache"
os.environ["MYPY_CACHE_DIR"] = str(cache_dir)

DEPS = ['lief', 'mypy', 'pyzmq']

# Only .py files in test/functional and contrib/devtools have type annotations
# enforced.
MYPY_FILES_ARGS = ['git', 'ls-files', 'test/functional/*.py', 'contrib/devtools/*.py']


def check_dependencies():
    for dep in DEPS:
        try:
            metadata(dep)
        except PackageNotFoundError:
            print(f"Skipping Python linting since {dep} is not installed.")
            exit(0)

    if not shutil.which('ty'):
        print("Skipping Python linting since ty is not installed.")
        exit(0)


def main():
    check_dependencies()

    mypy_files = subprocess.check_output(MYPY_FILES_ARGS, text=True).splitlines()
    mypy_args = ['mypy', '--show-error-codes'] + mypy_files

    try:
        subprocess.check_call(mypy_args)
        subprocess.check_call(['ty', 'check'])
    except subprocess.CalledProcessError:
        exit(1)


if __name__ == "__main__":
    main()
