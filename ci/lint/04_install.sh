#!/usr/bin/env bash
#
# Copyright (c) 2018-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C

export CI_RETRY_EXE="/ci_retry --"

set -euxo pipefail

pushd "/"

${CI_RETRY_EXE} apt-get update
# Lint dependencies:
# - curl/xz-utils (to install shellcheck)
# - git (used in many lint scripts)
# - gpg (used by verify-commits)
${CI_RETRY_EXE} apt-get install -y curl xz-utils git gpg

# Install uv
UV_VERSION=0.5.6
curl --proto '=https' --tlsv1.2 -LsSf https://github.com/astral-sh/uv/releases/download/$UV_VERSION/uv-installer.sh | sh
source /root/.local/bin/env

# Configure venv
export PYTHON_VENV="/.venv"
uv venv $PYTHON_VENV

# Set up permanent environment variables
cat > /etc/profile.d/bitcoin-lint-env.sh << EOF
export PATH="${PYTHON_VENV}/bin:/root/.local/bin:${PATH}"
export LINT_RUNNER_PATH="/lint_test_runner"
EOF
chmod +x /etc/profile.d/bitcoin-lint-env.sh
source /etc/profile.d/bitcoin-lint-env.sh

# Test python
command -v python3
python3 --version

# Install project dependencies
uv pip install -r /pyproject.toml

if [ ! -d "${LINT_RUNNER_PATH}" ]; then
  ${CI_RETRY_EXE} apt-get install -y cargo
  (
    cd "/test/lint/test_runner" || exit 1
    cargo build
    mkdir -p "${LINT_RUNNER_PATH}"
    mv target/debug/test_runner "${LINT_RUNNER_PATH}"
  )
fi

SHELLCHECK_VERSION=v0.8.0
curl -sL "https://github.com/koalaman/shellcheck/releases/download/${SHELLCHECK_VERSION}/shellcheck-${SHELLCHECK_VERSION}.linux.x86_64.tar.xz" | \
    tar --xz -xf - --directory /tmp/
mv "/tmp/shellcheck-${SHELLCHECK_VERSION}/shellcheck" /usr/bin/

MLC_VERSION=v0.19.0
MLC_BIN=mlc-x86_64-linux
curl -sL "https://github.com/becheran/mlc/releases/download/${MLC_VERSION}/${MLC_BIN}" -o "/usr/bin/mlc"
chmod +x /usr/bin/mlc

popd || exit
