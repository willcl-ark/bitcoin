#!/usr/bin/env bash
# Copyright (c) The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit.

materialize_depends() {
    local marker source_prefix staging target_prefix

    : "${BASEPREFIX:?not set}"
    : "${DEPENDS_STORE:?not set}"
    : "${HOST:?not set}"

    source_prefix="${DEPENDS_STORE}/prefix"
    target_prefix="${BASEPREFIX}/${HOST}"
    marker="${target_prefix}/.guix-depends-store"

    if [ ! -d "$source_prefix" ]; then
        echo "ERR: ${source_prefix} does not exist"
        return 1
    fi

    if [ -L "$target_prefix" ]; then
        echo "ERR: ${target_prefix} is a symbolic link"
        return 1
    fi

    if [ -e "$target_prefix" ]; then
        if [ -f "$marker" ] && [ "$(cat "$marker")" = "$DEPENDS_STORE" ]; then
            return 0
        fi

        echo "ERR: ${target_prefix} already exists and was not materialized from ${DEPENDS_STORE}"
        return 1
    fi

    mkdir -p "$BASEPREFIX"
    staging="${BASEPREFIX}/.${HOST}.guix-depends.$$"
    if [ -e "$staging" ]; then
        echo "ERR: staging path already exists: ${staging}"
        return 1
    fi

    mkdir -p "$staging"
    cp -a --no-preserve=ownership "${source_prefix}/." "$staging/"
    chmod u+w "$staging"
    printf '%s\n' "$DEPENDS_STORE" > "${staging}/.guix-depends-store"
    mv "$staging" "$target_prefix"
}
