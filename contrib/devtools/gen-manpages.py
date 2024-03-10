#!/usr/bin/env python3
# Copyright (c) 2022 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
import os
import subprocess
import sys
import tempfile
from collections import namedtuple

bin = namedtuple("bin", ["path", "name", "description"])
vbin = namedtuple("bin", ["path", "name", "description", "version", "copyright"])

BINARIES: list[bin] = [
    bin(
        path="src/bitcoind",
        name="bitcoind(1)",
        description="The Bitcoin Core daemon (bitcoind) is a headless program that connects to the Bitcoin network to validate and relay addresses, transactions and blocks.\n\nIt provides the backbone of the Bitcoin network's security and is used for wallet services, transaction broadcasting, and block creation in a headless environment or as part of a server setup."
    ),
    bin(path="src/bitcoin-cli",
        name="bitcoin-cli(1)",
        description="The bitcoin-cli utility provides a command line interface to interact with a bitcoind/bitcoin-qt RPC service.\n\nIt allows users to query network information, manage wallets, create or broadcast transactions, and control the bitcoind server."
    ),
    bin(
        path="src/bitcoin-tx",
        name="bitcoin-tx(1)",
        description="The bitcoin-tx tool is used for creating and modifying bitcoin transactions."
    ),
    bin(
        path="src/bitcoin-wallet",
        name="bitcoin-wallet(1)",
        description="This bitcoin-wallet utility is a tool for managing Bitcoin wallets.\n\nIt allows for creating new wallets, restoring wallets from backups, salvaging wallets and getting wallet information without the need for a bitcoind/bitcoin-qt instance."
    ),
    bin(
        path="src/bitcoin-util",
        name="bitcoin-util(1)",
        description="The bitcoin-util tool is used to grind proof of work on a hex-formatted bitcoin block header."
    ),
    bin(
        path="src/qt/bitcoin-qt",
        name="bitcoin-qt(1)",
        description="The bitcoin-qt application provides a graphical interface for interacting with Bitcoin Core. You can optionally specify a BIP21 [URI] using the BIP21 URI format.\n\nIt combines the core functionalities of bitcoind with a user-friendly interface for wallet management, transaction history, and network statistics.\n\nIt is suitable for desktop users preferring a graphical over a command-line interface."
    ),
]

# Paths to external utilities.
git = os.getenv('GIT', 'git')
help2man = os.getenv('HELP2MAN', 'help2man')

# If not otherwise specified, get top directory from git.
topdir = os.getenv('TOPDIR')
if not topdir:
    r = subprocess.run([git, 'rev-parse', '--show-toplevel'], stdout=subprocess.PIPE, check=True, text=True)
    topdir = r.stdout.rstrip()

# Get input and output directories.
builddir = os.getenv('BUILDDIR', topdir)
mandir = os.getenv('MANDIR', os.path.join(topdir, 'doc/man'))

# Verify that all the required binaries are usable, and extract copyright
# message in a first pass.
versions: list[vbin] = []
for bin in BINARIES:
    abspath = os.path.join(builddir, bin.path)
    try:
        r = subprocess.run([abspath, "--version"], stdout=subprocess.PIPE, check=True, text=True)
    except IOError:
        print(f'{abspath} not found or not an executable', file=sys.stderr)
        sys.exit(1)
    # take first line (which must contain version)
    verstr = r.stdout.splitlines()[0]
    # last word of line is the actual version e.g. v22.99.0-5c6b3d5b3508
    verstr = verstr.split()[-1]
    assert verstr.startswith('v')
    # remaining lines are copyright
    bitcoin_copyright = r.stdout.split('\n')[1:]
    assert bitcoin_copyright[0].startswith('Copyright (C)')

    versions.append(vbin(*bin, verstr, bitcoin_copyright))

if any(vbin.version.endswith('-dirty') for vbin in versions):
    print("WARNING: Binaries were built from a dirty tree.")
    print('man pages generated from dirty binaries should NOT be committed.')
    print('To properly generate man pages, please commit your changes (or discard them), rebuild, then run this script again.')
    print()

with tempfile.NamedTemporaryFile('w', suffix='.h2m') as footer:
    # Create copyright footer, and write it to a temporary include file.
    # Copyright is the same for all binaries, so just use the first.
    footer.write('[COPYRIGHT]\n')
    footer.write('\n'.join(versions[0].copyright).strip())
    footer.write('\n')
    footer.flush()

    # Call the binaries through help2man to produce a manual page for each of them.
    for vbin in versions:
        # Create a description section for all binaries
        with tempfile.NamedTemporaryFile("w+", suffix=".h2m") as description:
            # the `=` before DESCRIPTION here overrides any other description
            # that is parsed by help2man
            description.write("[=DESCRIPTION]\n")
            description.write(vbin.description)
            description.write("\n")
            description.flush()
            outname = os.path.join(mandir, os.path.basename(vbin.path) + ".1")
            print(f"Generating {outname}…")
            subprocess.run([help2man, "-N", "--name=" + vbin.name, "--version-string=" + vbin.version, "--include=" + footer.name, "--include=" + description.name, "-o", outname, vbin.path], check=True)
