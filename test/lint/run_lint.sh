#!/usr/bin/env bash
export LC_ALL=C

set -euo pipefail

error() {
    echo "ERROR: $1" >&2
    exit 1
}

GIT_DIR=$(git rev-parse --git-dir)

WORKTREE_ROOT=""
if [[ "$GIT_DIR" == *".git/worktrees/"* ]]; then
    echo "Detected git worktree..."
    WORKTREE_ROOT=$(echo "$GIT_DIR" | sed 's/\.git\/worktrees\/.*/\.git/')
    echo "Worktree root: $WORKTREE_ROOT"
fi

echo "Building Docker image..."
DOCKER_BUILDKIT=1 docker build \
    -t bitcoin-linter \
    --file "./ci/lint_imagefile" \
    ./ || error "Docker build failed"

echo "Running linter..."
DOCKER_ARGS=(
    --rm
    -v "$(pwd):/bitcoin"
    -it
)

if [[ -n "$WORKTREE_ROOT" ]]; then
    # If in a worktree, mount both the current directory and the git root
    DOCKER_ARGS+=(--mount "type=bind,src=$WORKTREE_ROOT,dst=$WORKTREE_ROOT,readonly")
fi

RUST_BACKTRACE=full docker run "${DOCKER_ARGS[@]}" bitcoin-linter
