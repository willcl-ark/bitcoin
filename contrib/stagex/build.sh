#!/usr/bin/env bash
# Copyright (c) 2025-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
set -euo pipefail

command -v docker >/dev/null || {
    echo "Error: docker not found"
    exit 1
}
command -v git >/dev/null || {
    echo "Error: git not found"
    exit 1
}

git rev-parse --git-dir >/dev/null 2>&1 || {
    echo "Error: not in git repo"
    exit 1
}

DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(git rev-parse --show-toplevel)"
DOCKERFILE="$DIR/Dockerfile"
GIT_REVISION="$(git describe --always --abbrev=12 --dirty --exclude '*')"

HOSTS="${HOSTS:-x86_64-linux-musl}"
PLATFORM="linux/amd64"

export DOCKER_BUILDKIT=1
export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-$(echo | git -c log.showSignature=false log --format=%at -1 2>/dev/null | head -1)}"

set_host_flags() {
    local host="$1"
    HOST_CFLAGS="-O2 -g -fdebug-prefix-map=/bitcoin=. -fmacro-prefix-map=/bitcoin=."
    HOST_CXXFLAGS="$HOST_CFLAGS"
    HOST_LDFLAGS="-static"

    case "$host" in
    *mingw*)
        HOST_CFLAGS+=" -fno-ident"
        HOST_LDFLAGS="-Wl,--no-insert-timestamp"
        ;;
    *darwin*) unset HOST_CFLAGS HOST_CXXFLAGS ;;
    esac
}

build_docker_image() {
    local host="$1"
    shift

    echo "Building Bitcoin Core with Stagex Container"
    echo "Dockerfile:     $DOCKERFILE"
    echo "Docker context: $REPO_ROOT"
    echo "Git revision:   $GIT_REVISION"
    echo "Host target:    $host"
    echo "SOURCE_DATE_EPOCH: $SOURCE_DATE_EPOCH"

    docker buildx build -f "$DOCKERFILE" "$REPO_ROOT" \
        --build-arg host="$host" \
        --build-arg SOURCE_DATE_EPOCH="$SOURCE_DATE_EPOCH" \
        --build-arg GIT_REVISION="$GIT_REVISION" \
        --build-arg CFLAGS="$HOST_CFLAGS" \
        --build-arg CXXFLAGS="$HOST_CXXFLAGS" \
        --build-arg LDFLAGS="$HOST_LDFLAGS" \
        --build-arg CMAKE_POSITION_INDEPENDENT_CODE="OFF" \
        --platform "$PLATFORM" \
        --tag "bitcoin-stagex-$host:$GIT_REVISION" \
        --load \
        "$@"

    echo
    echo "Build completed successfully for $host!"
    echo
}

extract_artifacts() {
    local container_id="$1"
    local output_base="$2"
    local host="$3"

    mkdir -p "$output_base/$host"

    # Copy tarballs to output directory
    docker cp "$container_id:/opt/dist/bitcoin-${GIT_REVISION}-${host}.tar.gz" "$output_base/$host/"
    docker cp "$container_id:/opt/dist/bitcoin-${GIT_REVISION}-${host}-debug.tar.gz" "$output_base/$host/"

    # Create SHA256SUMS.part file for this host
    pushd "$output_base/$host" >/dev/null
    sha256sum ./*.tar.gz >SHA256SUMS.part
    popd >/dev/null

    echo
    echo "Build artifacts created in: $output_base/$host"
    echo "Files:"
    ls -la "$output_base/$host"
}

OUTPUT_BASE="stagex-build-$GIT_REVISION/output"

for host in $HOSTS; do
    echo
    echo "========================================"
    echo "Building for host: $host"
    echo "========================================"

    # Set host-specific flags
    set_host_flags "$host"

    # Build the image
    build_docker_image "$host" "$@"

    # Extract artifacts
    CONTAINER_ID=$(docker create "bitcoin-stagex-$host":"$GIT_REVISION" sh)
    extract_artifacts "$CONTAINER_ID" "$OUTPUT_BASE" "$host"
    docker rm "$CONTAINER_ID" >/dev/null
done

echo
echo "========================================"
echo "All builds completed successfully!"
echo "========================================"
echo
echo "StageX Build Hashes:"
uname -m
find "stagex-build-$GIT_REVISION/output/" -type f -print0 | env LC_ALL=C sort -z | xargs -r0 sha256sum
