# libbitcoinkernel

This guide documents architectural boundaries for the `libbitcoinkernel` extraction — the ongoing effort to isolate Bitcoin Core's consensus and validation logic into a standalone library with minimal dependencies.

## Scope

- `libbitcoinkernel` exposes block/transaction validation via the API defined in `src/kernel/bitcoinkernel.h`.
- Treat the library interface as evolving; consult the [library design
  document](../doc/design/libraries.md) and the headers under
  [`src/kernel/`](../src/kernel/) before changing boundaries.

## Library boundaries

- **No networking or p2p dependencies.** Kernel-level code must not introduce dependencies on p2p or networking modules. Strict separation of concerns applies at the library boundary.
- **No node-internal types in kernel-facing headers.** Avoid including `chain.h`, `CBlockIndex` internals, or other node-level types in kernel-facing index or option headers. Use custom options structs/classes that return configuration settings instead.
- **Define kernel option defaults in kernel headers.** Modifiable default values for kernel option structs (e.g., `MemPoolOptions`) belong in the kernel header itself, not pulled from policy headers. This avoids circular dependencies between kernel and policy modules.

## Dependency placement

- **`util/` is for general-purpose primitives.** Place code in the `util` library only if it should **not** be depended on by the kernel. Code that is not yet used by the kernel, but *could* legitimately be used there, also belongs in `util` — not in `common/`.
- The `common/` library sits above the kernel; kernel code must not depend on it.

## Internal interface patterns

- Interfaces wrap functionality implemented in the owning component. Do not move substantial business logic into interface adapters.
- Prefer explicit dependency boundaries and avoid new global state.
- Do not add `NODISCARD` annotations to boilerplate kernel API forwarding wrappers (the `m_lib_handle->*` pattern in `bitcoinkernel.h`); they add no value.
