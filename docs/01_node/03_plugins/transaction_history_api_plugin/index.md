# Transaction History API Plugin

The legacy history API has been merged into `eosio::transaction_history_api_plugin`. Enable both storage and HTTP API plugins in `config.ini`:

```ini
plugin = eosio::transaction_history_plugin
plugin = eosio::transaction_history_api_plugin
```

It exposes the compatible `/v1/history/get_transaction`, `/v1/history/get_actions`, `/v1/history/get_transaction_count`, `/v1/history/get_key_accounts`, and `/v1/history/get_controlled_accounts` endpoints. Use the HTTP plugin's API-category controls and trusted listener configuration when exposing these endpoints.
