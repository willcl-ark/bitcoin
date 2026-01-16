#!/usr/bin/env bash
#
# Copyright (c) 2019-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C.UTF-8

export CONTAINER_NAME=ci-native-previous-releases
export CI_IMAGE_NAME_TAG="mirror.gcr.io/ubuntu:22.04"
# Use minimum supported python3.10 and gcc-12, see doc/dependencies.md
export PACKAGES="gcc-12 g++-12 python3-zmq python3-pip wget"
export PIP_PACKAGES="pycapnp"
export CI_CMAKE_INSTALL="true"
export CMAKE_VERSION="3.24.3"
export CMAKE_URL="https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-x86_64.tar.gz"
export DEP_OPTS="CC=gcc-12 CXX=g++-12"
# Run extended tests so that coverage does not fail, but exclude the very slow dbcrash
export CI_LIMIT_STACK_SIZE=1
export DOWNLOAD_PREVIOUS_RELEASES="true"
