#!/usr/bin/env python3
# Copyright (c) 2015-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Utilities for doing coverage analysis on the RPC interface.

Provides a way to track which RPC commands are exercised during
testing.
"""

import os

from .authproxy import AuthServiceProxy
from typing import Optional

REFERENCE_FILENAME = 'rpc_interface.txt'
COVERAGE_FILE_PREFIX = 'coverage.'


class AuthServiceProxyWrapper():
    """
    An object that wraps AuthServiceProxy to record specific RPC calls.

    """
    def __init__(self, auth_service_proxy_instance: AuthServiceProxy, rpc_url: str, coverage_logfile: Optional[str]=None):
        """
        Kwargs:
            auth_service_proxy_instance: the instance being wrapped.
            rpc_url: url of the RPC instance being wrapped
            coverage_logfile: if specified, write each service_name
                out to a file when called.

        """
        self.auth_service_proxy_instance = auth_service_proxy_instance
        self.rpc_url = rpc_url
        self.coverage_logfile = coverage_logfile

    def __getattr__(self, name):
        return_val = getattr(self.auth_service_proxy_instance, name)
        if not isinstance(return_val, type(self.auth_service_proxy_instance)):
            # If proxy getattr returned an unwrapped value, do the same here.
            return return_val
        return AuthServiceProxyWrapper(return_val, self.rpc_url, self.coverage_logfile)

    def __call__(self, *args, **kwargs):
        """
        Delegates to AuthServiceProxy, then writes the particular RPC method
        called to a file.

        """
        return_val = self.auth_service_proxy_instance.__call__(*args, **kwargs)
        self._log_call()
        return return_val

    def _log_call(self):
        rpc_method = self.auth_service_proxy_instance._service_name

        if self.coverage_logfile:
            with open(self.coverage_logfile, 'a+') as f:
                f.write("%s\n" % rpc_method)

    def __truediv__(self, relative_uri):
        return AuthServiceProxyWrapper(self.auth_service_proxy_instance / relative_uri,
                                       self.rpc_url,
                                       self.coverage_logfile)

    def get_request(self, *args, **kwargs):
        self._log_call()
        return self.auth_service_proxy_instance.get_request(*args, **kwargs)

def get_filename(dirname, n_node):
    """
    Get a filename unique to the test process ID and node.

    This file will contain a list of RPC commands covered.
    """
    pid = str(os.getpid())
    return os.path.join(
        dirname, f"{COVERAGE_FILE_PREFIX}pid{pid}.node{n_node}.txt")


def write_all_rpc_commands(dirname: str, node: AuthServiceProxy) -> bool:
    """
    Write out a list of all RPC functions available in `bitcoin-cli` for
    coverage comparison. This will only happen once per coverage
    directory.

    Args:
        dirname: temporary test dir
        node: client

    Returns:
        if the RPC interface file was written.

    """
    filename = os.path.join(dirname, REFERENCE_FILENAME)

    if os.path.isfile(filename):
        return False

    help_output = node.help().split('\n')
    commands = set()

    for line in help_output:
        line = line.strip()

        # Ignore blanks and headers
        if line and not line.startswith('='):
            commands.add("%s\n" % line.split()[0])

    with open(filename, 'w') as f:
        f.writelines(list(commands))

    return True


def get_uncovered_rpc_commands(dirname: str) -> set[str]:
    """
    Return a set of currently untested RPC commands.
    """
    coverage_ref_filename = os.path.join(dirname, REFERENCE_FILENAME)
    coverage_filenames = set()
    all_cmds = set()
    # Consider RPC generate covered, because it is overloaded in
    # test_framework/test_node.py and not seen by the coverage check.
    covered_cmds = {'generate'}

    if not os.path.isfile(coverage_ref_filename):
        raise RuntimeError("No coverage reference found")

    with open(coverage_ref_filename, 'r', encoding='utf8') as coverage_ref_file:
        all_cmds.update(line.strip() for line in coverage_ref_file.readlines())

    for root, _, files in os.walk(dirname):
        for filename in files:
            if filename.startswith(COVERAGE_FILE_PREFIX):
                coverage_filenames.add(os.path.join(root, filename))

    for filename in coverage_filenames:
        with open(filename, 'r', encoding='utf8') as coverage_file:
            covered_cmds.update(line.strip() for line in coverage_file.readlines())

    return all_cmds - covered_cmds


def report_rpc_coverage(dirname: str) -> bool:
    """
    Print out RPC commands that were unexercised by tests.
    """
    uncovered = get_uncovered_rpc_commands(dirname)

    if uncovered:
        print("Uncovered RPC commands:")
        print("".join(f"  - {command}\n" for command in sorted(uncovered)))
        return False

    print("All RPC commands covered.")
    return True
