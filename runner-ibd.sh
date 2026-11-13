#!/usr/bin/env bash
# Prepare locally or on the runner; run only after the current job finishes.
set -euo pipefail

CORE_COMMIT=b414ddb2b77849f48f2cd991712dfa21b7cb301d
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
    run|reindex-chainstate)
        [[ $EUID == 0 ]] || { echo 'Run as root on runner-root.' >&2; exit 1; }
        [[ -x $WORK/build/bin/bitcoind ]]
        # Never stop a service that still has a benchmark in flight.
        if pgrep -x bitcoind; then
            echo 'A bitcoind is still running; wait for the current run to finish.' >&2
            exit 1
        fi
        results=$WORK
        extra_args=()
        if [[ $1 == reindex-chainstate ]]; then
            [[ -d $DATADIR/chainstate && -d $BLOCKSDIR/blocks ]]
            results=$(mktemp -d "$WORK/reindex-chainstate-$(date -u +%Y%m%dT%H%M%SZ)-XXXXXX")
            extra_args=(-reindex-chainstate -networkactive=0 -debuglogfile="$results/debug.log")
        else
            [[ ! -e $DATADIR && ! -e $BLOCKSDIR ]] || {
                echo 'Use fresh DATADIR and BLOCKSDIR paths for a timed IBD.' >&2; exit 1;
            }
        fi
        findmnt --mountpoint /data >/dev/null
        df -h / /data
        mkdir -p "$WORK"
        # status is assigned when the EXIT trap runs.
        # shellcheck disable=SC2154
        trap 'status=$?; {
            date --iso-8601=seconds
            printf "Run exit status: %s (0 means target height reached)\n" "$status"
            if [[ -f $results/time.txt ]]; then cat "$results/time.txt"; fi
        } > "$results/results.txt"' EXIT
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
        echo "Run results: $results"
        args=(-datadir="$DATADIR" -blocksdir="$BLOCKSDIR" -conf=/dev/null
              -chain=main -disablewallet -daemon=0 -server=0 -listen=0
              -par=16 -prevoutfetchthreads=16 -connectwindow=64
              -dbcache=35000 -prune=0 -stopatheight=975073 -printtoconsole=0 "${extra_args[@]}")
        {
            date --iso-8601=seconds
            git -C "$WORK/bitcoin" rev-parse HEAD
            git -C "$WORK/bix" rev-parse HEAD
            printf '%q ' "$WORK/build/bin/bitcoind" "${args[@]}"
            printf '\n'
            df -h "$DATADIR" "$BLOCKSDIR"
        } > "$results/run-info.txt"
        # Resolve the shell and time utility before starting the measured interval.
        nix shell "$TIME_FLAKE" --command time --version > "$results/time-version.txt"
        nix shell "$TIME_FLAKE" --command time -v -o "$results/time.txt" \
            "$WORK/build/bin/bitcoind" "${args[@]}" > "$results/console.log" 2>&1
        debug_log=$DATADIR/debug.log
        if [[ $1 == reindex-chainstate ]]; then debug_log=$results/debug.log; fi
        awk '/UpdateTip:/ {
            for (i=1; i<=NF; ++i)
                if ($i ~ /^height=/ && substr($i,8)+0 >= 975073) reached=1
        } END {exit !reached}' "$debug_log"
        echo "Reached height 975073. Timing: $results/time.txt"
        ;;
    *)
        echo "Usage: $0 prepare|run|reindex-chainstate"
        echo 'prepare: clone pinned sources and build without wallet support.'
        echo 'run: stop GitHub runner services and time fresh mainnet IBD.'
        echo 'reindex-chainstate: time rebuilding existing chainstate with networking disabled.'
        exit 1
        ;;
esac
