#!/usr/bin/env bash
#
# Copyright (c) 2018-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C

set -o errexit -o pipefail -o xtrace

export CI_RETRY_EXE="/ci_retry --"

pushd "/"

${CI_RETRY_EXE} apt-get update
# Lint dependencies:
# - cargo (used to run the lint tests)
# - curl/xz-utils (to install shellcheck)
# - git (used in many lint scripts)
# - gpg (used by verify-commits)
${CI_RETRY_EXE} apt-get install -y cargo curl xz-utils git gpg

# Install uv and python
curl -LsSf https://astral.sh/uv/install.sh | sh
# shellcheck disable=SC1091
source "$HOME/.local/bin/env"
uv python install "$(cat /.python-version)"
uv venv --python "$(cat /.python-version)" "$PYTHON_VENV"

# Instead of sourcing a venv activate script, just add venv to PATH.
# Otherwise shellcheck's `--check-sourced` fails on the activation script syntax
export PATH="$PYTHON_VENV/bin:$PATH"
command -v python3
python3 --version

${CI_RETRY_EXE} uv pip install \
  codespell==2.4.1 \
  lief==0.16.6 \
  mypy==1.4.1 \
  pyzmq==25.1.0 \
  ruff==0.5.5 \
  vulture==2.6

SHELLCHECK_VERSION=v0.8.0
curl -sL "https://github.com/koalaman/shellcheck/releases/download/${SHELLCHECK_VERSION}/shellcheck-${SHELLCHECK_VERSION}.linux.x86_64.tar.xz" | \
    tar --xz -xf - --directory /tmp/
mv "/tmp/shellcheck-${SHELLCHECK_VERSION}/shellcheck" /usr/bin/

MLC_VERSION=v0.19.0
MLC_BIN=mlc-x86_64-linux
curl -sL "https://github.com/becheran/mlc/releases/download/${MLC_VERSION}/${MLC_BIN}" -o "/usr/bin/mlc"
chmod +x /usr/bin/mlc

popd || exit
