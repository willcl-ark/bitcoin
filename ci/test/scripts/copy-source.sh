#!/usr/bin/env bash
#
# Copy source files to container's build directory
#
# Copyright (c) 2025-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

set -e

# Create the target directory
mkdir -p /ci_container_base

# Allow git to work with the mounted directory
git config --global --add safe.directory /bitcoin

# Copy only tracked files, excluding .git directory
cd /bitcoin
git ls-files | xargs -I{} cp --parents {} /ci_container_base/

# Also copy untracked but non-ignored files that might be needed for building
git ls-files --others --exclude-standard | xargs -I{} cp --parents {} /ci_container_base/ 2>/dev/null || true

# Make sure the test script exists in the expected location
if [[ ! -f "/ci_container_base/ci/test/scripts/test.sh" ]]; then
  echo "Error: Could not find ci/test/scripts/test.sh in the expected location"
  echo "Files copied to /ci_container_base/ci/test/:"
  ls -la /ci_container_base/ci/test/ || echo "Directory does not exist"
  exit 1
fi

# Ensure scripts are executable
find /ci_container_base -type f -name "*.sh" -exec chmod +x {} \;

echo "Source files copied to /ci_container_base successfully"
