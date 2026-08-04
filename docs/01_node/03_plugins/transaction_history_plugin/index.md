# Transaction History Plugin

The `eosio::transaction_history_plugin` records successful transaction and action history in RocksDB. Enable it in `config.ini`:

```ini
plugin = eosio::transaction_history_plugin
```

The database defaults to `transaction_history` under the node data directory. Its retention, trace, account-index, write-batch, API-response, and checkpoint disk limits are configurable with the corresponding `transaction-history-*` options documented by `funod --help`.

The plugin creates per-block rollback checkpoints in a sibling `transaction_history_checkpoints` directory and restores them when the accepted chain switches forks. Checkpoints below the irreversible boundary are removed, so disk use follows the actual reversible window rather than chain age.

On startup, the stored accepted-block ID is checked against the active chain.
Snapshot, replay, or branch mismatches clear stale history and external
checkpoints when automatic repair is enabled; otherwise recording is disabled
to avoid mixing branches.
