#!/usr/bin/env bash
#
# Copyright (c) The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C.UTF-8

export CONTAINER_NAME="ci-mac-native-fuzz"  # macos does not use a container, but the env var is needed for logging
export CI_OS_NAME="macos"
export NO_DEPENDS=1
export OSX_SDK=""
