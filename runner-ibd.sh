#!/usr/bin/env bash
# Prepare locally or on the runner; run only after the current job finishes.
set -euo pipefail

CORE_COMMIT=7d313e9322523798aee63728fb1c2003d87e1bad
BIX_COMMIT=eef14aeccfca9af88ab2a60487e71ab5fdcb123e
TIME_FLAKE=github:NixOS/nixpkgs/20b1ddd1aa5ace70c9468305030aa4f9ef79671b#time
WORK=${WORK:-$HOME/pr-201-ibd}
SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
CORE_SOURCE=${CORE_SOURCE:-$SCRIPT_DIR/pr-201.bundle}
DATADIR=${DATADIR:-$WORK/datadir}
BLOCKSDIR=${BLOCKSDIR:-/data/pr-201-ibd}
JOBS=${JOBS:-4}

case ${1:-} in
    prepare)
        mkdir -p "$WORK"
        if [[ ! -d $WORK/bitcoin ]]; then
            git clone --no-checkout "$CORE_SOURCE" "$WORK/bitcoin"
            git -C "$WORK/bitcoin" checkout --detach "$CORE_COMMIT"
        fi
        [[ $(git -C "$WORK/bitcoin" rev-parse HEAD) == "$CORE_COMMIT" ]]
        [[ -z $(git -C "$WORK/bitcoin" status --porcelain --untracked-files=no) ]]
        if [[ ! -d $WORK/bix ]]; then
            git clone https://github.com/bitcoin-dev-tools/bix "$WORK/bix"
            git -C "$WORK/bix" checkout --detach "$BIX_COMMIT"
        fi
        [[ $(git -C "$WORK/bix" rev-parse HEAD) == "$BIX_COMMIT" ]]
        [[ -z $(git -C "$WORK/bix" status --porcelain --untracked-files=no) ]]
        # shellcheck disable=SC2016
        nix develop "path:$WORK/bix#default" --command bash -euc '
            cmake -S "$1/bitcoin" -B "$1/build" -G Ninja \
                -DCMAKE_BUILD_TYPE=Release -DENABLE_WALLET=OFF \
                -DBUILD_GUI=OFF -DBUILD_TESTS=OFF -DBUILD_BENCH=OFF \
                -DENABLE_IPC=OFF
            cmake --build "$1/build" --target bitcoind --parallel "$2"
            mkdir -p "$1/version-check"
            "$1/build/bin/bitcoind" -datadir="$1/version-check" -nosettings -version
        ' bash "$WORK" "$JOBS" 2>&1 | tee "$WORK/build.log"
        nix shell "$TIME_FLAKE" --command time --version > "$WORK/time-version.txt"
        ;;
    run)
        [[ $EUID == 0 ]] || { echo 'Run as root on runner-root.' >&2; exit 1; }
        [[ -x $WORK/build/bin/bitcoind ]]
        # Never stop a service that still has a benchmark in flight.
        if pgrep -x bitcoind; then
            echo 'A bitcoind is still running; wait for the current run to finish.' >&2
            exit 1
        fi
        [[ ! -e $DATADIR && ! -e $BLOCKSDIR ]] || {
            echo 'Use fresh DATADIR and BLOCKSDIR paths for a timed IBD.' >&2; exit 1;
        }
        findmnt --mountpoint /data >/dev/null
        df -h / /data
        mkdir -p "$WORK"
        # status is assigned when the EXIT trap runs.
        # shellcheck disable=SC2154
        trap 'status=$?; {
            date --iso-8601=seconds
            printf "Run exit status: %s (0 means target height reached)\n" "$status"
            if [[ -f $WORK/time.txt ]]; then cat "$WORK/time.txt"; fi
        } > "$WORK/results.txt"' EXIT
        systemctl list-units --type=service --state=active --no-legend \
            'github*' 'actions.runner*' | awk '{print $1}' >> "$WORK/stopped-services.txt"
        sort -u -o "$WORK/stopped-services.txt" "$WORK/stopped-services.txt"
        mapfile -t services < "$WORK/stopped-services.txt"
        if ((${#services[@]})); then
            systemctl stop "${services[@]}"
        fi
        echo 'Runner services will remain stopped; see stopped-services.txt to restart them.'
        if pgrep -x bitcoind; then
            echo 'A bitcoind appeared while stopping services; aborting.' >&2; exit 1
        fi
        mkdir -p "$DATADIR" "$BLOCKSDIR"
        args=(-datadir="$DATADIR" -blocksdir="$BLOCKSDIR" -conf=/dev/null
              -chain=main -disablewallet -daemon=0 -server=0 -listen=0
              -dbcache=35000 -prune=0 -stopatheight=975073 -printtoconsole=0)
        {
            date --iso-8601=seconds
            git -C "$WORK/bitcoin" rev-parse HEAD
            git -C "$WORK/bix" rev-parse HEAD
            printf '%q ' "$WORK/build/bin/bitcoind" "${args[@]}"
            printf '\n'
            df -h "$DATADIR" "$BLOCKSDIR"
        } > "$WORK/run-info.txt"
        # Resolve the shell and time utility before starting the measured interval.
        nix shell "$TIME_FLAKE" --command time --version > "$WORK/time-version.txt"
        nix shell "$TIME_FLAKE" --command time -v -o "$WORK/time.txt" \
            "$WORK/build/bin/bitcoind" "${args[@]}" > "$WORK/console.log" 2>&1
        awk '/UpdateTip:/ {
            for (i=1; i<=NF; ++i)
                if ($i ~ /^height=/ && substr($i,8)+0 >= 975073) reached=1
        } END {exit !reached}' "$DATADIR/debug.log"
        echo "Reached height 975073. Timing: $WORK/time.txt"
        ;;
    *)
        echo "Usage: $0 prepare|run"
        echo 'prepare: clone pinned sources and build without wallet support.'
        echo 'run: stop GitHub runner services and time fresh mainnet IBD.'
        exit 1
        ;;
esac
