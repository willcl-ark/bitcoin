#!/usr/bin/env bash
#
# Copyright (c) 2023-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C.UTF-8

export CI_IMAGE_NAME_TAG="mirror.gcr.io/ubuntu:24.04"
export CONTAINER_NAME=ci-native-tidy
export TIDY_LLVM_V="21"
export APT_LLVM_V="${TIDY_LLVM_V}"
export PACKAGES="clang-${TIDY_LLVM_V} libclang-${TIDY_LLVM_V}-dev llvm-${TIDY_LLVM_V}-dev libomp-${TIDY_LLVM_V}-dev clang-tidy-${TIDY_LLVM_V} jq libevent-dev libboost-dev libzmq3-dev systemtap-sdt-dev qt6-base-dev qt6-tools-dev qt6-l10n-tools libqrencode-dev libsqlite3-dev libcapnp-dev capnproto"
export NO_DEPENDS=1
