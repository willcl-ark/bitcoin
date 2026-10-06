#!/usr/bin/env python3
# Copyright (c) The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
"""Special script to run each bench sanity check
"""
import shlex
import subprocess
import os
from pathlib import Path

from test_framework.test_framework import BitcoinTestFramework
from test_framework.test_profile import enabled as profile_enabled


class BenchSanityCheck(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 0  # No node/datadir needed
        self.setup_clean_chain = True

    def setup_network(self):
        pass

    def skip_test_if_missing_module(self):
        self.skip_if_no_bitcoin_bench()

    def add_options(self, parser):
        parser.add_argument(
            "--bench",
            default=".*",
            help="Regex to filter the bench to run (default=%(default)s)",
        )

    def run_test(self):
        cmd = self.get_binaries().bench_argv() + [
            f"-filter={self.options.bench}",
            "-sanity-check",
        ]
        env = None
        if profile_enabled():
            profile_path = Path(os.getenv("BITCOIN_TEST_PROFILE_FILE") or
                                Path.cwd() / "test-profiles" / f"{Path(__file__).name}.json").with_suffix(".bench.json")
            profile_path.parent.mkdir(parents=True, exist_ok=True)
            env = os.environ.copy()
            env["BITCOIN_BENCH_PROFILE_FILE"] = str(profile_path)
        self.log.info(f"Starting: {shlex.join(cmd)}")
        subprocess.run(cmd, check=True, env=env)
        self.log.info("Success!")


if __name__ == "__main__":
    BenchSanityCheck(__file__).main()
