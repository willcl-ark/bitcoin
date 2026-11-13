# PR 201 timed IBD

`runner-ibd.sh` has two explicit stages. Nothing invokes SSH. Only run these
commands on runner-root once its current benchmark has finished.

The default workspace and datadir are `/root/pr-201-ibd` and
`/root/pr-201-ibd/datadir` on the boot SSD. The blocks directory is
`/data/pr-201-ibd` on the other SSD. The script creates fresh directories and
never removes benchmark data. It reports free space but does not enforce a
space budget. Pruning is disabled; monitor available space during the run.

The local source snapshot is `/home/will/.cache/pr-201-ibd/pr-201.bundle`, containing commit
`f8895cf09db2a12a273ddd6f9774dd9c2f2a26ec`. Copy it alongside the script on the runner. Regenerating the same snapshot:

```bash
git update-ref refs/benchmarks/pr-201-peer-run f8895cf09db2a12a273ddd6f9774dd9c2f2a26ec
git bundle create /home/will/.cache/pr-201-ibd/pr-201.bundle refs/benchmarks/pr-201-peer-run
```

After the active benchmark finishes, copy the files from this checkout:

```bash
scp -F ~/.ssh/config runner-ibd.sh /home/will/.cache/pr-201-ibd/pr-201.bundle runner-root:/root/
ssh -F ~/.ssh/config runner-root
```

On the runner, prepare the build, then run in a persistent terminal session:

```bash
cd /root
JOBS=4 ./runner-ibd.sh prepare
./runner-ibd.sh run
```

`prepare` clones the source bundle, clones the pinned Bix shell, and builds
Release `bitcoind` with wallet, GUI, tests, benchmarks, and IPC disabled.
Bix includes `-march=native`, so build again on the runner rather than copying
the locally built executable. JOBS defaults to 4 and can be increased once the
runner is idle. The time utility is resolved during preparation too.

`run` refuses to proceed if any bitcoind process exists. It records and stops
active `github*` and `actions.runner*` services, then checks for bitcoind again.
It leaves those services stopped even after failure. Restart them deliberately
when finished, using the names in `/root/pr-201-ibd/stopped-services.txt`, e.g.:

```bash
systemctl start github-runner-ax52.service
```

The node uses mainnet peer discovery, `-dbcache=35000`, `-prune=0`,
`-par=16`, `-prevoutfetchthreads=16`, `-connectwindow=64`,
`-disablewallet`, and `-stopatheight=975073`. It reads no bitcoin.conf and
runs in the foreground with no RPC server or incoming connections.
Default assumevalid remains enabled. This measures network IBD with the
branch defaults; it is not a full historical script-validation measurement.

`results.txt` records the finish timestamp, overall exit status, and timing
output on completion or failure. Exit status zero means the height check passed.
For unattended operation use tmux or nohup, for example:

```bash
nohup ./runner-ibd.sh run > /root/pr-201-launch.log 2>&1 < /dev/null &
```

GNU time records elapsed wall time, CPU time, and maximum resident memory in
`/root/pr-201-ibd/time.txt`. The interval includes startup and shutdown, but
excludes cloning, building, and Nix setup. `run-info.txt` records commits,
arguments, launch timestamp, and free space. Node logs are in `datadir/debug.log`
and `console.log`. Success requires exit status zero and a logged UpdateTip
height of at least 975073. The node can process extra blocks during shutdown.

WORK, CORE_SOURCE, DATADIR, BLOCKSDIR, and JOBS are environment overrides.
For another IBD run, choose fresh workspace/datadir/blocksdir paths; the script
will not resume an existing run or silently time a partial sync.

## Timed chainstate reindex

After a successful IBD, keep both data directories and use the same binary:

```bash
cd /root
nohup ./runner-ibd.sh reindex-chainstate > /root/pr-201-reindex-launch.log 2>&1 < /dev/null &
```

This retains the IBD parameters and adds `-reindex-chainstate -networkactive=0`.
It rebuilds chainstate from the existing block files without peer connections.
Timing includes startup and shutdown. The original IBD outputs remain intact;
a new `reindex-chainstate-<UTC timestamp>-<suffix>` directory under the workspace
contains `run-info.txt`, `debug.log`, `console.log`, `time.txt`, and `results.txt`.
The launch log prints that directory. The height check uses this new debug log.

The peering comparison uses local branch snapshot `f8895cf09db`, including its
4 GiB block-cache default and IBD peer changes. The baseline IBD outputs are
archived remotely in `ibd-eb76028a6dd-20261113T094027Z/` under the workspace;
the baseline offline reindex keeps its existing separate results directory.
