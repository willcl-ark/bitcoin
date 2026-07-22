# Build system

This guide covers build system conventions for Bitcoin Core contributors. For first-time build instructions, see [`doc/build-*.md`](../doc/build-unix.md) and [`depends/README.md`](../depends/README.md).

## CMake workflow

- Use `cmake -B <build_dir>` for configuration and `cmake --build <build_dir>` for compilation. Do not invoke the underlying build tool directly.
- The normal build type is `RelWithDebInfo`; use `Debug` when needed.
- When CMake configure fails, **delete the entire build directory** before re-running to avoid stale cache artifacts.

## Style and structure

- Keep CMakeLists.txt source lists **alphabetically sorted** and use **2-space indentation**.
- Use `if(VAR)` (bare variable, no space before parenthesis) for boolean checks.
- Prefer `cmake_path()` over `get_filename_component()` (CMake ≥ 3.20). Use `string(REPEAT ...)` for repeated characters.
- Place external library targets after internal library targets in `target_link_libraries()`. Mark rarely-changed variables with `mark_as_advanced()`.
- Use `add_custom_command` with `target_sources` for code generation, not `add_custom_target` with `add_dependencies`.
- **Avoid** `file(GLOB)` and `if(EXISTS ...)` in CMakeLists.txt; use explicit file lists.
- Use `COMMAND_ERROR_IS_FATAL` in `execute_process()`. Explicitly specify `NO_SOURCE_PERMISSIONS` or `USE_SOURCE_PERMISSIONS` in every `configure_file()` call.
- Do **not** set variables starting with `CMAKE_` in project files; they are reserved for users.

## Feature detection

- **Prefer testing feature support** (e.g., CMake check-compiles, C++ feature-test macros `__cpp_lib_*`) over hardcoded compiler-version checks, which are brittle across patched compilers and non-GCC variants.
- Use specific feature-test macros rather than raw `__cplusplus` minimum values when guarding C++ standard library features.

## Include what you use (IWYU)

- Every file must include what it directly uses. Do **not** rely on transitive includes or IWYU pragma export for standard library headers.
- Reserve `// IWYU pragma: export` for documented facade headers (`compat.h`) or implementation headers providing the main header's symbols.
- Suppress IWYU false positives with `// IWYU pragma: keep` and a comment referencing the upstream issue.

## Generated data & build-time artifacts

- Store source binary data files in the repository and generate headers at compile time via `add_custom_command`. Omit header guards on data-holding generated headers intentionally (duplicate inclusion should error).
- Add comments explaining **why** workaround compiler flags or build configuration hacks exist, with conditions for removal (e.g., minimum version bump, upstream fix).

## Presets and CI

- CI presets belong in a `/ci/` directory, separate from developer presets.
- Generic developer presets should enable all optional features by default but avoid platform-specific options that cannot work everywhere.
- Prefer CI configuration in `CMakePresets.json` over embedding in YAML or scripts, so settings are reusable locally.

## Dependencies and subtrees

- Do **not** modify upstream files in `depends/` or vendored subtrees (`leveldb/`, `libmultiprocess/`) directly. Submit changes upstream first.
- Keep `depends/` generic and assumption-free; place release-specific hardening flags in **Guix**, not hardcoded in depends.
- When enabling compiler hardening (libc++, glibcxx), ensure only one hardening mode macro is active per translation unit and that vendor defaults are not silently downgraded.

## Testing and environment

- Use the `ENVIRONMENT` test property in CMake for environment variables required by tests, so developers can reproduce tests locally.
- Build system defaults must be consistent with release binary configuration, CI testing, and end-user documentation. Changing a default requires simultaneous documentation updates.
