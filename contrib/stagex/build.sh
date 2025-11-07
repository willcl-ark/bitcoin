#!/usr/bin/env bash
# Copyright (c) 2025 Bitcoin Core developers
set -e

DIR="$( cd "$( dirname "$0" )" && pwd )"
REPO_ROOT="$(git rev-parse --show-toplevel)"
DOCKERFILE="$DIR/Dockerfile"
GIT_REVISION="$(git describe --always --abbrev=12 --dirty --exclude '*')"

HOST="${HOST:-x86_64-linux-musl}"
PLATFORM="linux/amd64"

# Set SOURCE_DATE_EPOCH early
export DOCKER_BUILDKIT=1
export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-$(echo | git -c log.showSignature=false log --format=%at -1 2>/dev/null | head -1)}"

echo
echo "Building Bitcoin Core with Stagex"
echo "Dockerfile:     $DOCKERFILE"
echo "Docker context: $REPO_ROOT"
echo "Git revision:   $GIT_REVISION"
echo "Host target:    $HOST"
echo "SOURCE_DATE_EPOCH: $SOURCE_DATE_EPOCH"
echo

docker build -f "$DOCKERFILE" "$REPO_ROOT" \
    --no-cache \
    --build-arg host="$HOST" \
    --build-arg SOURCE_DATE_EPOCH="$SOURCE_DATE_EPOCH" \
    --platform "$PLATFORM" \
    --tag "bitcoin-stagex:$GIT_REVISION" \
    --load \
    "$@"
