# Copyright (c) The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

# See test/lint/README.md for usage.

FROM python:3.10.14-alpine as builder

ENV DEBIAN_FRONTEND=noninteractive
ENV LC_ALL=C.UTF-8
ENV LINT_RUNNER_PATH="/lint_test_runner"
ENV MLC_BIN=mlc-x86_64-linux
ENV MLC_VERSION=v0.19.0

COPY --from=ghcr.io/astral-sh/uv:alpine /usr/local/bin/uv /bin/
COPY --from=koalaman/shellcheck:v0.8.0 /bin/shellcheck /bin/
COPY ./.python-version /.python-version
COPY ./ci/requirements.txt /requirements.txt
COPY ./ci/lint/container-entrypoint.sh /entrypoint.sh
COPY ./test/lint/test_runner /test/lint/test_runner

RUN apk add --no-cache rust cargo curl git xz && \
    # Install mlc
    curl -sL "https://github.com/becheran/mlc/releases/download/${MLC_VERSION}/${MLC_BIN}" -o "/usr/bin/mlc" && \
    chmod +x /usr/bin/mlc && \
    # Setup Python
    uv venv && \
    . /.venv/bin/activate && \
    uv pip install -r /requirements.txt && \
    export PATH="/.venv/bin:${PATH}" && \
    # Build test runner
    cd /test/lint/test_runner && \
    cargo build --release

FROM python:3.10.14-alpine
COPY --from=builder /usr/bin/mlc /usr/bin/
COPY --from=builder /bin/shellcheck /usr/bin/
COPY --from=builder /test/lint/test_runner/target/release/test_runner ${LINT_RUNNER_PATH}/
COPY --from=builder /.venv /.venv
COPY --from=builder /entrypoint.sh /entrypoint.sh

ENV PATH="/.venv/bin:${PATH}"
WORKDIR /bitcoin
RUN chmod 755 /entrypoint.sh
ENTRYPOINT ["/entrypoint.sh"]
