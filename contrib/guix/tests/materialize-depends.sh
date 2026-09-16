#!/usr/bin/env bash
# Copyright (c) The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit.
set -euo pipefail

# shellcheck source=../libexec/materialize-depends.sh
source "$(dirname "${BASH_SOURCE[0]}")/../libexec/materialize-depends.sh"

test_dir="$(mktemp -d)"
trap 'chmod -R u+w "$test_dir"; rm -rf -- "$test_dir"' EXIT
export BASEPREFIX="$test_dir/depends"
export DEPENDS_STORE="$test_dir/store"
export HOST=x86_64-linux-gnu
mkdir -p "$DEPENDS_STORE/prefix/include"
printf 'header\n' > "$DEPENDS_STORE/prefix/include/header.h"
ln -s header.h "$DEPENDS_STORE/prefix/include/link.h"
chmod -R a-w "$DEPENDS_STORE"

materialize_depends
cmp "$DEPENDS_STORE/prefix/include/header.h" "$BASEPREFIX/$HOST/include/header.h"
test "$(readlink "$BASEPREFIX/$HOST/include/link.h")" = header.h
test "$(cat "$BASEPREFIX/$HOST/.guix-depends-store")" = "$DEPENDS_STORE"
materialize_depends

export DEPENDS_STORE="$test_dir/other-store"
mkdir -p "$DEPENDS_STORE/prefix"
if materialize_depends; then
    echo 'Accepted an existing prefix from a different store output' >&2
    exit 1
fi
export HOST=unknown-prefix
mkdir "$BASEPREFIX/$HOST"
if materialize_depends; then
    echo 'Accepted an unmanaged prefix' >&2
    exit 1
fi
export HOST=symlink-prefix
ln -s "$BASEPREFIX/unknown-prefix" "$BASEPREFIX/$HOST"
if materialize_depends; then
    echo 'Accepted a symlink prefix' >&2
    exit 1
fi
echo 'depends materialization checks passed'
