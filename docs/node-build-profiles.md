# Node packaging, build variants, and runtime roles

FLON ships one node program with two build variants, four runtime role presets,
and a separate operator tools package. This keeps consensus behavior identical
between roles while excluding history-indexing code and dependencies from nodes
that do not need them.

Build variants do not change consensus rules, chain identity, the active
producer schedule, or protocol-feature activation. Runtime configuration does
not add code that was excluded at build time.

## Package model

| Release archive | Contents | Intended use |
| --- | --- | --- |
| `flon-node-<version>-<platform>` | Core `funod` binary and all role presets | BP, RPC, and P2P seed nodes |
| `flon-node-history-<version>-<platform>` | History-enabled `funod` binary and all role presets | History RPC, explorer, and indexer services |
| `flon-tools-<version>-<platform>` | `fucli`, `fuwal`, `flon-util`, `funod-state-inspect`, and `funod-state-checkpoint` | Operators, automation, and maintenance hosts |

Every archive is produced in both `.tar.gz` and `.zip` formats. Packages never
contain private keys, wallet data, producer names, peer lists, or environment
secrets. Supply those through deployment-specific configuration or a secret
manager.

`flon-tools` is not a node distribution. It contains five executable programs,
not four: four native binaries plus the Python 3
`funod-state-checkpoint` executable script.

## Build variants

Select the variant with `FLON_NODE_VARIANT`:

| Variant | Chain/P2P/RPC/producer engine | Transaction history | State history (SHiP) | Trace API |
| --- | --- | --- | --- | --- |
| `core` (default) | yes | no | no | no |
| `history` | yes | yes | yes | yes |

BP, RPC, and seed are operational roles, not different binaries. The producer
engine remains linked in `core` because it participates in normal block
processing; a node only produces blocks when producer identity and signing are
configured.

The deprecated `FLON_NODE_PROFILE` setting is accepted for one release as a
compatibility bridge: `bp` and `rpc` map to `core`, while `full` and `history`
map to `history`. New build automation must use `FLON_NODE_VARIANT`.

Example core build and archives (retain platform-specific toolchain flags):

```sh
cmake -S . -B build-core -DCMAKE_BUILD_TYPE=Release \
  -DFLON_NODE_VARIANT=core
cmake --build build-core --target flon-node-package flon-tools-package -j4
```

Example history build:

```sh
cmake -S . -B build-history -DCMAKE_BUILD_TYPE=Release \
  -DFLON_NODE_VARIANT=history
cmake --build build-history --target flon-node-package -j4
```

Portable archives are written to `<build-directory>/packages/`. To build a node
without compiling client tools, set `FLON_BUILD_CLIENT_TOOLS=OFF`; the
`flon-tools-package` target is then intentionally unavailable.

## Runtime roles

Release archives contain presets under `config/`. Copy the selected preset to
the node's configuration directory and layer deployment-specific values on top.

| Role | Build variant | Preset | Responsibilities |
| --- | --- | --- | --- |
| BP | `core` | `bp.ini` | Validate transactions and produce blocks |
| RPC | `core` | `rpc.ini` | Serve current chain-state and transaction-submission APIs |
| Seed | `core` | `seed.ini` | Provide stable P2P discovery and relay |
| History | `history` | `history.ini` | Record/query transaction history and serve state-history streams |

Example launch:

```sh
./bin/funod --config-dir ./config --config bp.ini \
  --data-dir /var/lib/flon/bp
```

The preset is a baseline, not a complete production configuration. Operators
must add chain-specific genesis/snapshot settings, P2P peers, resource limits,
logging, and monitoring. Only a BP configuration may contain `producer-name`
and `signature-provider`. Keep signing keys in a protected secret provider; do
not write them into a release archive or container image.

RPC account queries require `enable-account-queries = true` and the chain state
database. Pruning block logs does not remove that requirement.

## Why history is a separate variant

If a `core` binary is given a configuration containing a history plugin,
startup fails because that plugin was not compiled and registered. This is
deliberate: it prevents a BP or lightweight RPC deployment from silently taking
on indexing work.

The `history` binary can run without enabling every history plugin, but enabling
them increases disk usage, RocksDB writes, CPU/memory consumption, replay time,
and recovery time. History data is an index rather than consensus state; loss
does not invalidate the chain, but the index must be rebuilt from retained chain
data or a suitable snapshot/recovery source.

Do not colocate a public history service with a production BP process. Expensive
queries and indexing I/O can add latency and disk contention to block
production. Use separate processes and data directories, preferably separate
hosts. Bind history/RPC APIs to a private interface or loopback address when
possible and expose them through an authenticated, rate-limited gateway.

`transaction_history_plugin` records locally queryable transaction/action
history. `state_history_plugin` provides the state-history stream used by
external indexers. `trace_api_plugin` serves trace data. Enable only the
capabilities required by the service.

## Feature overrides

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

Overrides are intended for controlled development and specialized builds. A
deployment that enables history features on top of `core` is operationally a
custom history-capable build and should be packaged, tested, and operated as
such rather than treated as a BP/RPC core artifact.

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
