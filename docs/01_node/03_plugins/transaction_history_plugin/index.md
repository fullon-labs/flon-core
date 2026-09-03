# Transaction History Plugin

The `eosio::transaction_history_plugin` records successful transaction and action history in RocksDB. Enable it in `config.ini`:

```ini
plugin = eosio::transaction_history_plugin
```

The database defaults to `transaction_history` under the node data directory. Its retention, trace, account-index, write-batch, API-response, queue, and checkpoint disk limits are configurable with the corresponding `transaction-history-*` options documented by `funod --help`.

The ordered database writer uses a bounded queue. Required chain events are never discarded when this queue fills: `funod` applies backpressure and slows replay or synchronization until RocksDB catches up. The defaults are:

```ini
transaction-history-max-queue-tasks = 10000
transaction-history-max-queue-bytes = 268435456
```

Increasing these limits may absorb short write-latency spikes but does not increase sustained indexing throughput. Size them according to available memory; replay is intentionally limited to the database writer's sustainable rate so transaction history remains complete.

The plugin creates per-block rollback checkpoints in a sibling `transaction_history_checkpoints` directory and restores them when the accepted chain switches forks. Checkpoints below the irreversible boundary are removed, so disk use follows the actual reversible window rather than chain age.

On startup, the stored accepted-block ID is checked against the active chain.
Snapshot, replay, or branch mismatches clear stale history and external
checkpoints when automatic repair is enabled; otherwise recording is disabled
to avoid mixing branches.

Monitor `history_queue_tasks`, `history_queue_bytes`, `history_queue_max_tasks`, `history_queue_max_bytes`, `history_healthy`, and `history_gap_block` through `get_performance_metrics`. A healthy replay may keep the queue near its configured limit while backpressure is active; queue saturation alone is not a history gap.
