#!/usr/bin/env bash
#
# Copyright (c) 2019-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C.UTF-8

export CI_IMAGE_NAME_TAG="mirror.gcr.io/debian:trixie"
export CONTAINER_NAME=ci-native-fuzz-valgrind
export PACKAGES="libevent-dev libboost-dev libsqlite3-dev valgrind libcapnp-dev capnproto"
export NO_DEPENDS=1
export FUZZ_TESTS_CONFIG="--valgrind"
