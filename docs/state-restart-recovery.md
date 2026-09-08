# State shutdown and recovery hardening

Chainbase state and RocksDB transaction history are separate databases. The
history fixes do not make the mmap-based chain state crash-recoverable.

## What this change does

- Synchronously save data before clearing Chainbase's dirty header. A failed
  data or header flush is not silently accepted.
- Preserve/restore dirty on save failure where the device still permits it.
  Failure to persist even the dirty header is possible on a failing device;
  the node must not announce success in that case.
- Destructors catch persistence errors, release resources, and record a sticky
  failure status. Normal `funod` shutdown returns **3** if state persistence
  failed; opening an already-dirty database still returns **2**. Other fatal
  errors may use their existing nonzero exit codes.
- Flush only the header when changing dirty status, instead of synchronizing
  the entire mapping again. The data itself is still synchronously saved first.
- Propagate soft-dirty page-save errors and retain zero-overwrite correctness
  when saving private/heap mappings.
- Report the Chainbase close duration and the chain-plugin shutdown stage.
- Check snapshot stream writes/flush/close, sync the completed temporary file,
  and sync the parent directory after pending/final promotion on POSIX.
  Snapshot publication still waits for irreversibility. Windows directory
  durability is not covered by the POSIX implementation.

The second stage adds offline physical checkpoints and an explicit restore
helper, described below. The third stage adds explicit, validated continuation
of interrupted restores (`resume`), without recopying verified staged files.
No dirty-header bypass, automatic state deletion,
WAL, online checkpoint capture, consensus changes, or automatic BP failover
are added. Normal startup's existing block-log replay is reused after restore.
Its no-undo-session fast path now verifies that the first replay block links
to the state head, then commits the checkpoint's formerly reversible undo
records before replay. This prevents `cannot set revision while there is an
existing undo stack` when replay advances a restored physical state.

## Stop safely

Use the existing service name in a Compose override:

```yaml
services:
  funod: # replace with the existing Compose service name
    stop_signal: SIGTERM
    stop_grace_period: 5m
```

Five minutes is a starting allowance, not a guarantee. Measure shutdown under
load and allow for storage latency; ensure the host/service manager's shutdown
timeout is not shorter. `docker stop --timeout 300 CONTAINER` is the equivalent
explicit timeout for an individual stop. Stop first, verify completion and
exit status, then recreate/start the node. Do not run forced removal/restarts
as a shortcut or automatically delete `state` when exit code 2/3 is seen.

The container's startup script should finish with `exec funod ...` (preserving
all current chain/BP options). A shell pipeline such as `funod | tee ...` needs
explicit signal forwarding and waiting; prefer the node's logger plus Docker
logging. Do not change signing keys or production configuration in this step.

Check `docker inspect` for PID, stop timeout, OOM status and mounts; inspect
actual `ps` output as the configured entrypoint alone does not show whether the
script eventually calls `exec`. A current `OOMKilled=false` does not rule out a
previous container's OOM event. Exit 137 alone does not prove OOM either.

### Read-only m1 check, 2026-09-08

Both `funod_bp_mainnet` and `funod_bp_testnet` ran v0.8.2 with `StopTimeout=120`
and current `OOMKilled=false`. Both containers' PID 1 was `funod`.
The `/mnt/data1` filesystem had about 456 GiB available. No snapshot `*.bin`
files were found within three levels of the two scoped node data directories;
an external/custom snapshot location was not ruled out. Both configs specified
`chain-state-db-size-mb=655360`: this is configured mapped capacity, not actual
resident memory usage. Do not switch such nodes to heap/locked/private mode
without measuring memory, storage and shutdown costs. No remote settings were
changed and neither BP was restarted.

## Recent recovery points: bounded pilot first

Use an observation node on the same chain where possible. Snapshot generation
still performs chain-state serialization and can affect BP latency; a durable
snapshot adds synchronization I/O. Establish the time/space cost before any
permanent schedule. Do not publish producer/snapshot management RPC publicly.

The helper is **preview-only by default**, targets loopback RPC (use an SSH
tunnel for a remote node), and plans a finite 2-10 snapshots rather than
unbounded disk growth. It detects its existing schedule and does not duplicate
or replace it. It neither deletes snapshots nor changes block retention.

```sh
python3 scripts/plan_recovery_snapshots.py --url http://127.0.0.1:8888 \
  --block-spacing 1000 --count 3
# After inspecting the displayed chain ID/range and available disk space:
python3 scripts/plan_recovery_snapshots.py --url http://127.0.0.1:8888 \
  --block-spacing 1000 --count 3 --apply
```

`1000` is an example, not a production recommendation. Spacing counts actual
blocks, so elastic production does not imply a fixed hourly interval. The
helper requires `chain_api_plugin` and `producer_api_plugin` on the private RPC
endpoint. A successful registration is not proof that any snapshot exists.
After the pilot finishes, inspect the finalized files and test a restore before
establishing an ongoing retention/scheduling policy.

Only use finalized `snapshot-<block-id>.bin`, not `.incomplete-*` or `.pending-*`.
Keep multiple verified generations and a separate failure-domain copy. Preserve
enough continuous block history after the selected snapshot to cover the
recovery gap. Nodes pruning all block logs must arrange an archive source; a
recent snapshot alone does not provide the missing subsequent blocks.

Test restoration in an isolated directory with P2P and production disabled,
checking chain ID, snapshot block identity and height, accounts and replay.
Never overwrite a live BP's state to test recovery. Preserve `safety.dat` and
other signing/finalizer safety records; do not roll them backward with a chain
state backup. Do not run two BPs with the same signing identity simultaneously.

## Regression checks

```sh
cmake --build build --target funod funod-state-inspect chainbase_test chainbase_persistence_test \
  snapshot_file_test history_smoke_driver -j4
ctest --test-dir build \
  -R '^(chainbase_test|chainbase_persistence_test|snapshot_file_test)$' --output-on-failure
python3 tests/node_shutdown_smoke.py build/bin/funod \
  --driver build/plugins/transaction_history_plugin/history_smoke_driver \
  --inspector build/bin/funod-state-inspect
python3 tests/state_checkpoint_tests.py
```

The fault-injection executable recompiles Chainbase with test-only flush hooks;
the production library has no injection hooks. Tests cover mapped/private/heap
save and clean-marker failures. The node smoke test uses only a disposable
development chain: commit a native account creation, restart on SIGINT/SIGTERM,
create a finalized snapshot, refuse dirty state after SIGKILL, then restore the
snapshot while preserving the dirty test directory. It also checks that the
recovery-plan preview does not mutate the node, repeated application does not
duplicate an active schedule, and the bounded pilot completes. This does not emulate
physical power loss or replace Linux BP load/fork/replay testing.

## Offline physical checkpoints (second stage)

`funod-state-checkpoint` (source: `scripts/state_checkpoint.py`, Python 3.9+ on
Linux/macOS; no third-party Python modules) works with the
matching `funod-state-inspect` executable and new node operation locks. Stop the
service cleanly and disable restart policies before copying. Do not use an old
node binary during this procedure: it does not honor the new stable locks or
interrupted-restore markers. The native node now needs write access to the
state/blocks parent directories to maintain those locks.
Run the helper as the node's data-file owner: new checkpoint/replacement files
are private (mode 0600), and the tool does not guess a container UID or chown
files. Check ownership/mount permissions before restarting a service.

This first implementation deliberately supports only the standard
`DATA/state`, `DATA/blocks/reversible` directory layout without symlinked
subdirectories. Recovery requires an **unpruned active `blocks.log` containing
the checkpoint head** and the continuous irreversible tail. Partitioned logs
whose anchor has moved into retained/archive files, empty logs, and pruned logs
are refused; there is no automatic download, archive merge, or retention change.
The checkpoint head may still be reversible at capture, but restoration is
refused until its full ID is found in the current irreversible log. A checkpoint
on an abandoned fork will therefore not be restored.

Creation copies only these three coordinated files after clean shutdown:

- `state/shared_memory.bin`
- `state/chain_head.dat`
- `blocks/reversible/fork_db.dat`

The inspector checks the Chainbase environment/version/clean marker, revision,
chain ID and chain-head identity without consuming the head file. It also uses
the native v3 fork decoder to check fork structure and that it contains the
state head; older/future fork-file formats are refused. Protocol-feature checks
still occur in normal controller startup. A manifest
records these values and SHA-256 checksums of allocated extents. Sparse copies
avoid reading large holes where the filesystem supports `SEEK_DATA/SEEK_HOLE`;
verification also checks that omitted ranges contain no nonzero data. On a
filesystem without sparse-seek support this falls back to a full logical scan,
which can be expensive for a 640 GiB state capacity. Files and directories are
synchronized before publishing a new generation. Failed staging directories are
kept for inspection, never automatically promoted or deleted. Checksums detect
accidental corruption, not an attacker who can rewrite both files and manifest.

Example commands (replace paths with the actual standard-layout data directory):

```sh
# Service already stopped successfully; checkpoint parent already exists.
python3 scripts/state_checkpoint.py create --data-dir /srv/funod/data \
  --checkpoint /srv/checkpoints/clean-001 --inspector build/bin/funod-state-inspect
python3 scripts/state_checkpoint.py verify --checkpoint /srv/checkpoints/clean-001 \
  --inspector build/bin/funod-state-inspect

# After an abnormal exit, with the service and its restart policy disabled:
# First inspect the read-only recovery plan.
python3 scripts/state_checkpoint.py restore --data-dir /srv/funod/data \
  --checkpoint /srv/checkpoints/clean-001 --inspector build/bin/funod-state-inspect
# Apply that recovery only after confirming chain, checkpoint and replay gap.
python3 scripts/state_checkpoint.py restore --data-dir /srv/funod/data \
  --checkpoint /srv/checkpoints/clean-001 --inspector build/bin/funod-state-inspect --apply
```

Restore is **preview-only unless `--apply` is present**. It requires a dirty
existing state; it will not roll back a clean node or guess how to repair a
missing/incompatible database. Preflight verifies the checkpoint, full anchor
block ID, chain ID and every subsequent block-header link in the existing log
without opening it for writing or repairing its index. The native replay still
validates/deserializes the block payloads at startup. Restart remains a separate
operator action; this helper never starts the node or enables production.

All replacements are staged and verified on the target filesystem first.
Each original core file is renamed to a unique `.pre-restore-*` sibling, then
the replacement is installed. The block log/index, `finalizers/safety.dat`,
signing configuration, snapshots, history databases and other files are not
replaced. Preserve and audit those separately, especially before resuming a BP.
Run an isolated recovery/secondary-index check before production adoption.

The three-file swap is **journaled, not a single atomic rename**. Durable
`state.restore.pending` and `blocks.restore.pending` journals live in the data
directory. A partial swap or sync failure leaves at least one marker; the new
node refuses startup, even if a new shared-memory file looks clean. Do not
delete these markers or retry `restore` with another generation.

### Continue an interrupted restore (third stage)

Keep the service and its restart policy disabled, using the same node-data
owner and matching inspector as for the initial restore:

```sh
python3 scripts/state_checkpoint.py resume --data-dir /srv/funod/data \
  --inspector build/bin/funod-state-inspect
# Inspect per-file phases and the chain/replay range, then explicitly apply:
python3 scripts/state_checkpoint.py resume --data-dir /srv/funod/data \
  --inspector build/bin/funod-state-inspect --apply
```

`resume` is preview-only by default and does not accept `--checkpoint`: the
pending journal identifies the existing transaction, not a new recovery choice.
New **v2 journals** embed the validated checkpoint manifest and record original
file identities (device/inode/size/mtime). They are written and synced under a
temporary name before publication so an interrupted JSON write does not expose
a truncated recovery marker. An orphan `.writing-*` file is not a published
journal and is not consumed automatically.

Before any replacement, continuation validates both journals agree (or validates
the single remaining journal), restricts all paths to the three known core
files plus the recorded token suffix, rejects symlinks/hardlinks, verifies every
new file's checksums, and checks original-file identities. It also rechecks the
current irreversible log's chain, full anchor ID and continuity. It recognizes:

- `ready`: the new file is staged, and the original is still in place or was absent;
- `original_saved`: the original is backed up, and installation remains;
- `installed`: the new file is already in place, with its original backup preserved.

Only missing rename steps are performed. Already installed files are synced
again in case the preceding run stopped before directory sync. Missing markers
are re-established before applying any steps. Both markers are removed only
after the complete state/head/fork set passes native inspection and sync.
Continuation can itself be interrupted and resumed again without overwriting
the original backups. The original checkpoint mount need not remain attached:
the journal and verified staged/installed files are sufficient, but the current
block log and inspector are still required.

Ambiguous file combinations, changed originals/backups, mismatched journals,
missing/corrupt staged files, invalid paths, or unsupported formats stop without
replacing files. **Older v1 journals require manual reconciliation**; there is
no guessed migration, force switch, automatic rollback, or unattended startup.
Preserve all copies when diagnosis is required. If the final marker removal
completed before an error was reported, there may be no pending transaction;
verify the installed state rather than starting another rollback blindly.

There is no periodic online capture or automatic generation pruning. Keep
multiple verified generations and sufficient block-log retention; benchmark
copy time, storage and restoration on an observation node first. Physical
checkpoints avoid rebuilding state before their height, but do not eliminate
tail replay, protect against loss of the disk holding every copy, or replace
portable logical snapshots.

The isolated smoke test now captures a checkpoint **before** creating an
account, later kills the node, restores that checkpoint and verifies the account
after tail replay. It also checks refusal of live copy, clean-state rollback and
startup with a pending journal. A mismatching next-block header in a disposable
clone is refused by the controller before discarding undo history, verified by
comparing the saved undo revision range. It now also interrupts a real restore
after moving the dirty shared-memory file to backup, verifies the node refuses
startup and `resume` preview makes no replacements, disconnects the checkpoint
directory, and completes the restore via the CLI before checking account state
after replay. Filesystem tests cover all ten mutation boundaries (marker
publication, file renames and marker removal), repeated interruption, absent
original files, late-file corruption, changed backups, path tampering, symlinks,
disagreeing journals, and refusal of legacy journals. Existing data/sparse-hole
checksum and fsync-failure tests remain. These are not real power-loss or Linux
BP certification tests.

Chainbase changes reside inside the `libraries/chainbase` Git submodule. A
future release must publish that submodule commit and update the parent gitlink;
uncommitted submodule edits alone will not be included in a clean CI checkout.

## Shutdown metadata and voting safety

`chain_head.dat` and `fork_db.dat` are written to private temporary files on
the destination filesystem, closed with error checking, synced, and atomically
renamed with directory sync. Their shutdown failures set the same sticky
persistence status as Chainbase failures. State is not marked clean when that
status is set, and normal teardown returns exit code 3 instead of success.
Per-file replacement is not a multi-file transaction: the dirty flag remains
the barrier until all required metadata saves and the state save have succeeded.

BLS finalizer `safety.dat` uses the same durable replacement protocol, preserving
the existing version/CRC format and inactive-key safety records. Votes are only
published after persistence succeeds. A persistence failure disables subsequent
local voting for the life of that process; restarting requires operator review.
This adds synchronous storage latency to voting. Validate it on the actual BP
filesystem/device before deployment; no throughput improvement is claimed.

These changes do not implement automatic restoration, rollback, or data deletion.
Private staging directories left by an abrupt crash are not automatically used
as recovery sources.

Focused shutdown-failure checks (isolated dev chain, P2P disabled):

```sh
python3 tests/node_persistence_failure_smoke.py build/bin/funod
ctest --test-dir build -R '^(chainbase_persistence_test|snapshot_file_test|chain_safety_unit_tests)$' --output-on-failure
```
