#!/usr/bin/env bash
# Copyright (c) The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit.
export LC_ALL=C.UTF-8

setup_container_environment() {
    # Disable Guix ld auto-rpath behavior.
    export GUIX_LD_WRAPPER_DISABLE_RPATH=yes

    # Provide the conventional interpreter path inside the Guix container.
    [ -e /usr/bin ] || mkdir -p /usr/bin
    [ -e /usr/bin/env ] || ln -s --no-dereference "$(command -v env)" /usr/bin/env
}

clear_compiler_paths() {
    unset LIBRARY_PATH CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH OBJC_INCLUDE_PATH OBJCPLUS_INCLUDE_PATH
}
