#!/usr/bin/env bash
#
# Copyright (c) 2018-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C

set -o errexit -o pipefail -o xtrace

export CI_RETRY_EXE="/ci_retry"

pushd "/"

${CI_RETRY_EXE} apt-get update
# Lint dependencies:
# - cargo (used to run the lint tests)
# - curl/xz-utils (to install shellcheck)
# - git (used in many lint scripts)
# - gpg (used by verify-commits)
# - moreutils (used by scripted-diff)
${CI_RETRY_EXE} apt-get install -y cargo curl xz-utils git gpg moreutils

# Install Python and create venv using uv (reads version from .python-version)
uv python install
uv venv /python_env

export PATH="/python_env/bin:${PATH}"
command -v python3
python3 --version

uv pip install --python /python_env \
  lief==0.16.6 \
  mypy==1.18.2 \
  pyzmq==27.1.0 \
  vulture==2.14

SHELLCHECK_VERSION=v0.11.0
curl -sL "https://github.com/koalaman/shellcheck/releases/download/${SHELLCHECK_VERSION}/shellcheck-${SHELLCHECK_VERSION}.linux.x86_64.tar.xz" | \
    tar --xz -xf - --directory /tmp/
mv "/tmp/shellcheck-${SHELLCHECK_VERSION}/shellcheck" /usr/bin/

MLC_VERSION=v1
MLC_BIN=mlc-x86_64-linux
curl -sL "https://github.com/becheran/mlc/releases/download/${MLC_VERSION}/${MLC_BIN}" -o "/usr/bin/mlc"
chmod +x /usr/bin/mlc

popd || exit
