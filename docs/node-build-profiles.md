# Node build profiles and hardened API behavior

`FLON_NODE_PROFILE` selects optional code at build time. It does not change
consensus rules, chain identity, runtime producer configuration, or enable
account queries automatically.

| Profile | Chain/P2P/RPC/producer engine | Transaction history | SHiP | Trace |
| --- | --- | --- | --- | --- |
| `full` (default) | yes | yes | yes | yes |
| `bp` | yes | no | no | no |
| `rpc` | yes | no | no | no |
| `history` | yes | yes | yes | yes |

BP and RPC currently share the same minimal binary. The producer engine is a
dependency of block acceptance and remains linked for RPC nodes. Configure
producer names/signature providers only on BPs. RPC account queries require
`enable-account-queries = true` and the chain state database; pruning block logs
does not remove that requirement.

Example (retain the toolchain/dependency flags required by your platform):

```sh
cmake -S . -B build-rpc -DCMAKE_BUILD_TYPE=Release \
  -DFLON_NODE_PROFILE=rpc -DFLON_BUILD_CLIENT_TOOLS=OFF
cmake --build build-rpc --target funod -j4
```

Optional feature overrides accept `AUTO`, `ON`, or `OFF`:

- `FLON_WITH_TRANSACTION_HISTORY`
- `FLON_WITH_STATE_HISTORY`
- `FLON_WITH_TRACE_API`
- `FLON_WITH_SIGN_TRANSACTION` (AUTO is OFF)
- `FLON_WITH_TEST_CONTROL` (AUTO follows BUILD_TESTS; OFF in production builds)

AUTO is resolved again on every configure. Turning transaction history OFF
removes the RocksDB package requirement and its node linkage. Turning client
tools OFF excludes the client, wallet, and utility executables. For test
networks needing `test_control_api_plugin`, explicitly build with
`-DFLON_WITH_TEST_CONTROL=ON`; existing signing deployments likewise need
`-DFLON_WITH_SIGN_TRANSACTION=ON`.

## Management API migration

Wallet, producer-write, snapshot, network-write, and test-control APIs now reject
non-loopback listeners at initialization. Public chain RPC remains supported.
Use category listeners to keep management on loopback or Unix sockets, for
example with the corresponding plugins enabled:

```ini
http-server-address = http-category-address
http-category-address = chain_ro,0.0.0.0:8888
http-category-address = chain_rw,0.0.0.0:8888
http-category-address = producer_rw,127.0.0.1:8889
http-category-address = snapshot,127.0.0.1:8889
```

Container loopback is inside the container. An existing protected gateway
deployment which requires a non-loopback listener must explicitly set
`http-allow-insecure-management-api = true`. This flag does not add
authentication or encryption; the gateway and network restrictions must provide
them. The separate signing API remains Unix-socket-only even with this override.

## History limits and recovery

`get_transaction` and `get_actions` share one execution deadline across reads,
ABI decoding and JSON serialization, capped at 20 ms and the configured HTTP
response time. Synchronous RocksDB reads and JSON parsing are checked before and
after execution; they cannot be preempted mid-call. ABI timeout exceptions are
propagated. `transaction-history-max-api-response-size` covers the complete
response envelope, not just action payloads.

History responses include `history_status` with the last indexed height,
recording health, filter status, and earliest known account-index truncation
block. New transaction records include `account_index_complete`. The field is
absent on legacy records: absence means unknown, not complete. The global gap
marker only covers truncations observed after this upgrade. Configured filters
still restrict the recorded data.

The truncation marker is written with the block and its undo record. Rolling
back the affected branch also rolls back the marker. Undo replay groups up to
64 blocks with a 16 MiB target, retaining per-group atomic metadata/cursor
updates. A single larger undo record is kept atomic in its own group.

New normalized action and account-index rows are stored as JSON objects.
Readers, reference validation, and retention cleanup also accept legacy FC map
arrays (`[[key,value], ...]`); no offline rewrite is required. Existing public
response field types are unchanged.

A persisted history-recording gap no longer causes automatic deletion of the
history database on restart. Preserve it for investigation and rebuild a
separate history database by replaying retained chain data. Explicit
`transaction-history-force-clean` remains destructive and starts a new baseline;
it does not reconstruct earlier history by itself.

## Focused checks

Vote admission now bounds the total of queued, executing, and deferred remote
votes to 10,000 globally and 2,500 per connection. Local votes have 256 separate
slots and one dedicated processing thread, so remote queue saturation cannot
consume their admission budget or put them behind the remote FIFO. Saturation
drops excess votes with a rate-limited warning; it does not remove signature or
consensus verification. Deferred votes retain their reservations until processed
or expired, and new-block notifications are coalesced.

Account-history sequence reads distinguish missing counters from storage errors.
Malformed/exhausted counters and missing counters with existing account indexes
disable history recording through the existing gap-reporting path. Account index
inserts also check that the destination key does not exist, adding a point read
per account index to prevent overwrites from a stale counter. No automatic repair
or sequence reset is performed.

```sh
cmake --build build --target test_net_plugin http_plugin_unit_tests history_plugin_unit_tests history_smoke_driver -j4
ctest --test-dir build -R '^(test_net_plugin|http_plugin_unit_tests|history_plugin_unit_tests)$' --output-on-failure
python3 tests/node_profile_smoke.py build/bin/funod --history --produce \
  --driver build/plugins/transaction_history_plugin/history_smoke_driver
```

Use `--no-history` for a BP/RPC profile. These focused checks do not replace the
full chain/fork/replay integration suite before a production release.
Add `--produce` to verify block production with the public development key on
the isolated temporary chain (P2P disabled).
The optional test-only driver signs a native account-creation transaction and
checks transaction/action queries, then seeds a history gap in the temporary
database and verifies preservation across restart. It is never installed.

The `dev` install component now uses the generated version-check module and
only includes the generated unit-test fixtures/TestHarness when `BUILD_TESTS`
is enabled. The runtime-only profiles do not require those fixtures.
