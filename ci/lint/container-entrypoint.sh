#!/usr/bin/env bash
#
# Copyright (c) The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

export LC_ALL=C

# Source our environment
source /etc/profile.d/bitcoin-lint-env.sh

# Fixes permission issues when there is a container UID/GID mismatch with the owner
# of the mounted bitcoin src dir.
git config --global --add safe.directory /bitcoin

if [ -z "$1" ]; then
  bash -ic "./ci/lint/06_script.sh"
else
  exec "$@"
fi
