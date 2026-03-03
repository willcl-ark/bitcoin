#!/usr/bin/env python3
# Copyright (c) 2026 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
# /// script
# dependencies = ["jsonschema"]
# ///
"""Validate an OpenRPC document against the official meta-schema.

Usage:
    uv run contrib/devtools/check-openrpc.py doc/openrpc.json
    bitcoin-cli getopenrpcinfo | uv run contrib/devtools/check-openrpc.py -
"""
import json
import sys
import urllib.request

import jsonschema  # type: ignore[import-untyped]

METASCHEMA_URL = "https://github.com/open-rpc/meta-schema/releases/download/1.14.9/open-rpc-meta-schema.json"

if len(sys.argv) != 2:
    print(f"Usage: {sys.argv[0]} <openrpc.json | ->", file=sys.stderr)
    sys.exit(1)

path = sys.argv[1]
if path == "-":
    doc = json.load(sys.stdin)
else:
    with open(path) as f:
        doc = json.load(f)

with urllib.request.urlopen(METASCHEMA_URL) as resp:
    meta_schema = json.loads(resp.read())

jsonschema.validate(instance=doc, schema=meta_schema)
print("OpenRPC document is valid.")
