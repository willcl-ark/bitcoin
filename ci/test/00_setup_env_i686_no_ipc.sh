#!/usr/bin/env bash
#
# Copyright (c) 2020-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C.UTF-8

export HOST=i686-pc-linux-gnu
export CONTAINER_NAME=ci-i686-no-ipc
export CI_IMAGE_NAME_TAG="mirror.gcr.io/debian:trixie"
export CI_IMAGE_PLATFORM="linux/amd64"
export PACKAGES="llvm clang g++-multilib python3-zmq"
export DEP_OPTS="DEBUG=1 NO_IPC=1"
export CI_LIMIT_STACK_SIZE=1
