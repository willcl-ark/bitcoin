# CPack-Native Packaging Notes

This branch switches Windows installer generation from a custom NSIS template and
custom CMake deploy target to CPack's built-in NSIS generator.

It does **not** try to move all release packaging into CPack.

The resulting split is:

- `cmake --install`: defines the install tree and components
- `cpack -G NSIS64`: builds the Windows installer from that install tree
- Guix: still builds release archives, debug-symbol archives, and codesigning inputs
- macOS: still uses `macdeployqtplus` for the `.app` bundle and Guix-driven zip flow

That is the key design choice: use CPack where it fits well, and keep Guix/manual
packaging where reproducibility or environment constraints still dominate.

## What We Gain

### 1. One install model instead of a special Windows packaging path

Before this change, Windows packaging was driven by a separate NSIS template that
had to know which binaries and support files existed and where they lived.

With CPack, the Windows installer is built from the same `install()` rules that
already define the project install tree. That means:

- packaging follows the install tree automatically
- auxiliary files are installed once, not copied again later by special logic
- target/component ownership is visible at each target definition site
- installer contents are easier to reason about from CMake alone

In practice, this removes one of the biggest sources of drift: "the thing we
install" and "the thing we package for Windows" are no longer described in two
different systems.

### 2. Less project-specific NSIS maintenance

The old Windows flow depended on:

- a custom `setup.nsi.in`
- a helper CMake module to generate it
- a custom `deploy` target
- manual stripping/staging logic bound to that deploy target

That code now disappears in favor of:

- standard `install()`
- standard CPack metadata
- a generator-specific `cmake/CPackConfig.cmake`

This reduces the amount of custom packaging machinery we own and have to keep
compatible with future CMake changes.

### 3. Better layering

The new layering is cleaner:

- CMake answers: what gets installed, where, and in which component
- CPack answers: how to turn the install tree into an installer
- Guix answers: how to produce deterministic release artifacts

Those responsibilities were previously more entangled.

### 4. Easier local packaging workflow

For anyone building a Windows installer outside Guix, the workflow becomes more
standard:

```bash
cmake --build build
cpack --config build/CPackConfig.cmake -G NSIS64
```

or:

```bash
cmake --build build --target package
```

This is easier to understand than "build, then invoke the project's custom deploy
target which generates and runs a handwritten NSIS script."

### 5. Windows installer composition is now explicit

Because we now define `CPACK_COMPONENTS_ALL` explicitly, the installer only
contains the intended release-facing components: `bitcoin`, `bitcoind`,
`bitcoin_cli`, `bitcoin_tx`, `bitcoin_util`, `bitcoin_wallet`, `bitcoin_qt`,
and `auxiliary`.

Targets installed to `libexecdir` with their own components (`test_bitcoin`,
`bench_bitcoin`, `bitcoin-chainstate`, `bitcoin-node`, `bitcoin-gui`) are
**not** listed in `CPACK_COMPONENTS_ALL` and therefore do not appear in the
installer. The old installer explicitly included `test_bitcoin.exe` — this was
likely unintentional and is now corrected.

That gives a clear answer to "what belongs in the installer?" and avoids leaking
test/internal components into the NSIS UI or payload.

## What We Keep Deliberately

### 1. Guix remains the release-archive authority

We are **not** replacing Guix's manual `find | sort | tar/zip` archive creation
with CPack archive generators.

Reason:

- Guix already gives us deterministic archive creation
- changing that would risk reproducibility for little gain
- CPack archive output is not the problem we were trying to solve

This is an intentional non-goal.

### 2. Debug symbol handling stays in Guix

We still split debug symbols after install in the Guix flow.

Reason:

- it is already working
- it stays decoupled from per-target build rules
- it avoids pushing more release-specific behavior into the normal build

### 3. macOS DMG packaging stays deferred

We do not switch macOS packaging to CPack DragNDrop.

Reason:

- the current Linux-based Guix cross-build environment does not provide a useful
  DMG path for this
- `macdeployqtplus` remains the working bundle-assembly path
- Guix still owns the unsigned macOS zip and codesigning tarball flow

So the macOS refactor is limited to building `Bitcoin-Qt.app` as a normal build
artifact, not to adopting CPack for macOS releases.

## What We Lose

### 1. Some "anything-goes" NSIS flexibility

A handwritten NSIS template gives total control over every installer detail.
CPack exposes a lot of NSIS customization, but not arbitrary full-template control
without working through its hooks.

That means:

- some old NSIS-specific details are no longer first-class
- unusual installer behavior now has to fit through `CPACK_NSIS_*` hooks
- when we need custom NSIS commands, we must express them in the way CPack expects

In short: we gain structure, but lose some raw freedom.

### What this changes in practice

The old and new models are different in a very specific way.

Before:

- the whole Windows installer was authored in `share/setup.nsi.in`
- a helper CMake module generated that script
- we could place arbitrary NSIS commands anywhere we wanted
- the script directly named files, sections, registry writes, uninstall logic,
  and shortcut creation

Now:

- CPack owns the main NSIS template and generates `project.nsi`
- we configure it through `cmake/CPackConfig.cmake`
- custom behavior has to go through `CPACK_NSIS_*` variables and install
  components

That means the exact differences are:

- placement control is weaker: we no longer choose an arbitrary point in the
  script for every command, only the hook points CPack exposes
- script structure is no longer ours: CPack owns the page flow, component
  section structure, and uninstall skeleton
- packaging is now driven by the install tree: if something should be in the
  installer, the preferred place to express that is in `install()` rules and
  component membership, not in handwritten NSIS file-copy commands
- unusual behavior must be expressed as raw NSIS snippets inserted through
  variables like `CPACK_NSIS_CREATE_ICONS_EXTRA`,
  `CPACK_NSIS_EXTRA_INSTALL_COMMANDS`, and
  `CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS`

This branch already hit a few concrete examples of that:

- Start Menu network shortcuts are now injected through
  `CPACK_NSIS_CREATE_ICONS_EXTRA`
- `bitcoin:` URI registration is now injected through
  `CPACK_NSIS_EXTRA_INSTALL_COMMANDS`
- old `(64-bit)` registry and shortcut cleanup is now injected through the same
  install hook
- uninstall cleanup is injected through
  `CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS`

The constraints are also stricter than a handwritten template:

- these `*_EXTRA` values must be raw multiline NSIS snippets, not CMake lists
- NSIS resource paths must be valid at CPack time, not just at CMake configure
  time
- generator-specific metadata must be set in a way that survives into the
  generated `CPackConfig.cmake`

So the loss is not "we cannot customize the installer anymore." The loss is
"we no longer own the whole NSIS program directly." The gain is that installer
generation is now built on the same install/component model as the rest of the
project.

### 2. More dependence on CPack/NSIS generator behavior

The old template was our own code. The new path depends more on how CPack renders
NSIS scripts.

That creates a different class of risk:

- variable scoping matters more
- generator-specific variables must be shaped exactly as CPack expects
- CMake upgrades can change behavior in subtle ways

This branch already hit a few examples of that:

- `CPACK_*` metadata had to be set in the same scope as `include(CPack)`
- NSIS resource paths had to be resolved relative to `CMAKE_CURRENT_LIST_DIR`
- NSIS "extra commands" had to be provided as multiline strings, not CMake lists

So we traded handwritten template risk for CPack integration risk.

### 3. The old deploy target is gone

Anyone who previously relied on `deploy` for Windows packaging now needs to use
CMake/CPack packaging entry points instead.

This is not a loss in release engineering terms, but it is a workflow change.

## What We Restored To Avoid Regressions

Moving to CPack did not mean accepting large Windows-installer regressions.

This branch restores the important old behaviors through CPack's NSIS hooks:

- Start Menu testnet/signet/testnet4 shortcuts
- `bitcoin:` URI registration
- uninstall-before-install behavior
- cleanup of legacy `(64-bit)` registry and shortcut entries
- explicit installed icon metadata for Windows uninstall UI

That is an important point: the switch is not "accept whatever stock CPack does."
It is "use stock CPack as the base layer, and add back the small amount of custom
behavior we still care about.", kinda...

## What Changed Behaviorally

These are concrete differences between the old hand-written installer and the new
CPack-generated one. Some are intentional improvements, some are acceptable
trade-offs, and some may warrant further work.

### 1. Install directory layout

The old installer placed `bitcoin-qt.exe` and `bitcoin.exe` in `$INSTDIR` root
and daemon tools (`bitcoind.exe`, `bitcoin-cli.exe`, etc.) in `$INSTDIR\daemon\`.

The new installer places all binaries in `$INSTDIR\bin\`. This follows from
CPack consuming the standard `install()` rules, which use
`CMAKE_INSTALL_BINDIR`.

This is user-visible: any existing shortcuts, scripts, or documentation
referencing the old paths will need updating. The `bitcoin://` URI handler and
NSIS icon paths in `CPackConfig.cmake` already reference `bin\bitcoin-qt.exe`.

### 2. Auxiliary file locations

The old installer placed `readme.txt` and `bitcoin.conf` in `$INSTDIR` root and
`rpcauth/` under `$INSTDIR\share\rpcauth`.

The new installer places all auxiliary files under `$INSTDIR\share\bitcoin\`
(README, bitcoin.conf, rpcauth/) via the `auxiliary` component. This matches
the standard install tree layout.

### 3. Registry layout

The old installer used:

- `HKLM` for start menu registry (`MUI_STARTMENUPAGE_REGISTRY_ROOT HKLM`)
- `HKCU\SOFTWARE\Bitcoin Core` for `Path` and `StartMenuGroup`
- `HKCU\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Bitcoin Core` for
  Add/Remove Programs

CPack's NSIS64 generator manages its own registry entries under its own naming
conventions. The exact layout differs from the hand-rolled version. This could
affect the upgrade path from old-style installs — a user upgrading from a
pre-CPack installer may end up with both old and new registry entries.

The legacy `(64-bit)` cleanup commands are preserved in
`CPACK_NSIS_EXTRA_INSTALL_COMMANDS`, but there is no equivalent cleanup for the
non-suffixed old-style keys.

### 4. Uninstaller differences

The old uninstaller explicitly deleted:

- `debug.log` and `db.log` from `$INSTDIR`
- `SMSTARTUP\Bitcoin.lnk` (very old startup shortcut)
- individual files and registry keys by name

CPack generates its own uninstaller based on installed components. The explicit
`debug.log`/`db.log` cleanup is gone (those files live in `%APPDATA%`, not
`$INSTDIR`, so this is likely fine). The `SMSTARTUP` startup shortcut cleanup
is also gone (very old legacy).

### 5. No finish-page launch option

The old installer used `MUI_FINISHPAGE_RUN` to offer launching `bitcoin-qt.exe`
after install (via an `explorer.exe` workaround to avoid elevated launch).

CPack's generated installer does not include this. This is a minor UX regression.

### 6. RequestExecutionLevel

The old installer used `RequestExecutionLevel highest`, which requests admin
privileges if available but runs as the current user otherwise.

CPack's NSIS64 generator uses its own default. Worth verifying what the actual
generated value is and whether it matches the old behavior.

### 7. Man pages always installed

The old `INSTALL_MAN` option (default ON) controlled man page installation. That
option is removed; man pages are unconditionally installed as part of each
binary's component. On Windows this adds unused files to the installer but is
otherwise harmless.

> 21 files changed, 316 insertions(+), 381 deletions(-)

## Net Assessment

For Windows packaging, this is a good trade.

We gain:

- less custom packaging code
- a single authoritative install description
- better separation between install logic, installer generation, and release
  artifact generation
- a more standard local packaging workflow

We lose:

- some direct-template freedom
- some simplicity in debugging, because CPack generator behavior matters

And we accept some behavioral differences:

- binaries move from root/daemon to `bin\`
- auxiliary files move to `share\bitcoin\`
- registry layout changes (upgrade path from old installers unclear)
- no finish-page launch option
- `RequestExecutionLevel` may differ (needs verification)

But the current branch shows that the missing behaviors can be restored through
CPack's NSIS hooks, while still eliminating the custom template and deploy path.

## Short Version

If the question is "why use CPack at all?", the answer is:

Because Windows installer generation is exactly the kind of problem CPack is good
at, while Guix archive generation and macOS cross-packaging are exactly the kinds
of problems where keeping the existing specialized flows still makes sense.
