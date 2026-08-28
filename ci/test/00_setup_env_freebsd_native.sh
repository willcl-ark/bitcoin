#!/usr/bin/env bash
#
# Copyright (c) The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit.

export LC_ALL=C.UTF-8

export CONTAINER_NAME=ci_freebsd_native
export CI_OS_NAME=freebsd
export HOST=x86_64-unknown-freebsd
export BSD_RELEASE=15.1
export PACKAGES="shells/bash devel/bison devel/ccache devel/cmake devel/gmake devel/ninja curl devel/pkgconf devel/py-pip databases/py-sqlite3 lang/python3 net/py-pyzmq sysutils/lsof x11-fonts/fontconfig"
export PIP_PACKAGES="pycapnp"
export MAKE=gmake
export GOAL=install
export BITCOIN_CONFIG="\
 --preset=dev-mode \
 -DREDUCE_EXPORTS=ON \
 -DWITH_USDT=OFF \
"
