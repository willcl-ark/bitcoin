# PR 201 timed IBD

`runner-ibd.sh` has two explicit stages. Nothing invokes SSH. Only run these
commands on runner-root once its current benchmark has finished.

The default workspace and datadir are `/root/pr-201-ibd` and
`/root/pr-201-ibd/datadir` on the boot SSD. The blocks directory is
`/data/pr-201-ibd` on the other SSD. The script creates fresh directories and
never removes benchmark data. It reports free space but does not enforce a
space budget. Pruning is disabled; monitor available space during the run.

The local source snapshot is `/home/will/.cache/pr-201-ibd/pr-201.bundle`, containing commit
`7d313e9322523798aee63728fb1c2003d87e1bad`. Copy it alongside the script on the runner. Regenerating the same snapshot:

```bash
test "$(git rev-parse HEAD)" = 7d313e9322523798aee63728fb1c2003d87e1bad
git bundle create /home/will/.cache/pr-201-ibd/pr-201.bundle HEAD
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
For another run, choose fresh workspace/datadir/blocksdir paths; the script
will not resume an existing run or silently time a partial sync.
