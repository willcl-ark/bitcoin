# Mining

This guide covers block assembly, block templates, mining interfaces, and the boundary between mining policy and relay policy.

## Architecture

- `interfaces::Mining` wraps core mining functionality without owning it; the mining interface is a thin abstraction layer.
- Maintain separate configuration parameters for relay policy (`minrelaytxfee`) and mining policy (`blockmintxfee`). These two knobs enable coordinated policy transitions: to loosen policy, relax relay first then mining; to tighten, restrict mining first then relay. In steady state, they should match.
- Understand that `minrelaytxfee` is typically the binding constraint that limits which transactions are available for mining — `blockmintxfee` is often not the binding constraint because relay policy already filters low-fee transactions before they reach the mempool.
- For detailed interface documentation, see `src/interfaces/mining.h` (`../src/interfaces/mining.h`).

## Fee Policy Defaults

- Ensure the default `blockmintxfee` does not exceed the default `minrelaytxfee`. If mining policy were stricter than relay policy by default, a node would relay transactions it will never mine, creating a free relay problem.
- Defaults must yield a safe outcome for all users. When designing mining fee policy defaults, always consider the free relay problem: the node should not accept (relay) transactions that its own mining policy would never include in a block.
- Mining fee policy is distinct from relay policy; do not conflate them.
- See `src/policy/` (`../src/policy/`) for fee estimation and policy source.

## Block Assembly & Templates

- Block template creation and submission flows are defined in `src/rpc/mining.cpp` (`../src/rpc/mining.cpp`).
- For the mining RPC interface (e.g., `getblocktemplate`, `submitblock`), refer to the RPC documentation in `doc/` (`../doc/`).
- For functional testing of mining, prefer the test framework's `self.generate*` methods over direct mining RPC calls — these handle node synchronization automatically.

## Related Documentation

- Relay policy: `doc/policy/mempool.md` (`../doc/policy/mempool.md`)
- Transaction relay: `doc/policy/relay-policy.md` (`../doc/policy/relay-policy.md`)
