---
content_title: BLOCK_REFERENCE_DATA Protocol Feature Upgrade
---

`BLOCK_REFERENCE_DATA` enables the `get_recent_block_id` and
`get_last_irreversible_block_num` contract intrinsics. Protocol feature
activation is irreversible: after activation, every producer and validating
node must continue running a version that recognizes this feature.

## Required order

1. Build and stage the `v0.8.0-alpha` `funod`, `fucli`, and `fuwal` artifacts.
2. Upgrade non-producing validators and API nodes, then all current and standby
   block-producing nodes. Do not activate the feature while an old producer can
   still enter the active schedule.
3. Confirm that all nodes have the same chain ID, are synchronized, and advance
   LIB. Each private producer API must return the same feature digest for
   `BLOCK_REFERENCE_DATA`.
4. Confirm that `PREACTIVATE_FEATURE` is already active and the `flon` system
   contract ABI exposes the `activate` action.
5. Unlock a wallet containing authority for `flon@active`. Keep the producer API
   private and use a local or protected control endpoint.
6. Submit `flon::activate`, then wait for the activation block to become
   irreversible on every verification endpoint.
7. Only after step 6 deploy contracts that import the new intrinsics. For
   NumGuess, stop the old keeper before deployment and start the new keeper only
   after the contract and WebUI rollout is complete.

Before step 6, the rollout can be aborted and nodes can be downgraded. After
step 6 begins, do not downgrade a node to a version that does not recognize
`BLOCK_REFERENCE_DATA`.

## Script

The repository includes
[`scripts/activate-block-reference-data.sh`](../../../scripts/activate-block-reference-data.sh).
It resolves the feature digest from every producer rather than hard-coding it,
checks the feature's description digest, verifies the system contract and
`PREACTIVATE_FEATURE`, and waits until activation is irreversible.

Run the read-only preflight first. `PRODUCER_URLS` is a comma-separated list of
private endpoints with `producer_api_plugin` enabled:

```bash
NODE_URL=https://chain-api.example \
PRODUCER_URLS=http://producer-a.internal:8888,http://producer-b.internal:8888 \
./scripts/activate-block-reference-data.sh check
```

Record the chain ID printed by the preflight. Unlock the intended wallet using
the operator's normal secure command (for example, the configured `ut` command
on the test environment), then activate:

```bash
NODE_URL=https://chain-api.example \
PRODUCER_URLS=http://producer-a.internal:8888,http://producer-b.internal:8888 \
VERIFY_NODE_URLS=https://chain-api.example,http://validator.internal:8888 \
EXPECTED_CHAIN_ID=<exact-chain-id-from-preflight> \
WALLET_URL=unix:///tmp/fuwal.sock \
CONFIRM_ACTIVATION=BLOCK_REFERENCE_DATA \
./scripts/activate-block-reference-data.sh activate
```

FullOn can take up to 12 seconds to produce an idle block. The script polls
every 3 seconds and allows 900 seconds by default for the activation transaction,
the next activation block, and LIB advancement. Override
`ACTIVATION_TIMEOUT_SECONDS` only when the network's finality conditions justify
it.

An independent, read-only post-check does not require producer API access:

```bash
NODE_URL=https://chain-api.example \
VERIFY_NODE_URLS=https://chain-api.example,http://validator.internal:8888 \
./scripts/activate-block-reference-data.sh verify
```

The script never reads a wallet password or unlocks a wallet. It exits without
submitting a transaction when the feature is already active, making repeated
execution safe.
