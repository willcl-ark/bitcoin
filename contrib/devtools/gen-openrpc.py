#!/usr/bin/env python3
# Copyright (c) 2026 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

from openrpc import build_openrpc


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Generate an OpenRPC document from Bitcoin Core RPC metadata."
    )
    parser.add_argument(
        "--bitcoin-cli", default="bitcoin-cli", help="bitcoin-cli binary to invoke"
    )
    parser.add_argument(
        "--rpc-arg",
        action="append",
        default=[],
        help="Additional argument passed to bitcoin-cli",
    )
    parser.add_argument("--output", default="-", help="Output path (default: stdout)")
    parser.add_argument(
        "--input",
        help="Optional input file containing command descriptions JSON. If omitted, query bitcoin-cli help dump_all_command_descriptions.",
    )
    args = parser.parse_args()

    if args.input:
        command_descriptions = json.loads(Path(args.input).read_text(encoding="utf-8"))
    else:
        cmd = [args.bitcoin_cli, *args.rpc_arg, "help", "dump_all_command_descriptions"]
        try:
            output = subprocess.check_output(cmd, text=True)
        except subprocess.CalledProcessError as e:
            print(f"Failed to get command descriptions: {e}", file=sys.stderr)
            return 1
        command_descriptions = json.loads(output)

    openrpc = build_openrpc(command_descriptions)
    rendered = json.dumps(openrpc, indent=2, sort_keys=True) + "\n"
    if args.output == "-":
        sys.stdout.write(rendered)
    else:
        Path(args.output).write_text(rendered, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
