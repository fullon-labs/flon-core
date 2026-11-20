# Interop relayer RPC

See the [node setup and wallet workflow guide](../../docs/01_node/03_plugins/interop_relayer_api_plugin/index.md) for a complete configuration and RPC examples.

Enable with `plugin = eosio::interop_relayer_api_plugin`. The plugin uses the
signing protocol deployed by the `interopproxy` WASM contract and checks
`global.chain_id` against the node's own chain ID on every request.

| Option | Purpose |
| --- | --- |
| `interop-contract` | Proxy contract account, defaults to `interopproxy`. |
| `interop-relayer-permission` | Dedicated staked miner account and permission in `account@permission` form. Link a dedicated permission to the supported proxy actions. |
| `interop-signature-provider` | Provider specification for the relayer key. A `WALLET:` URL must include `/v1/wallet/sign_digest`. |
| `interop-signing-threads` | Dedicated signing threads. Default 2, range 1–8. |
| `interop-max-in-flight` | Pending submissions per node. Default 32, range 1–256. |
| `interop-submit-tcp` | Allow submission via TCP. Without it, submission is Unix socket only. |

Both signing options must be configured together. With neither option, only
preparation is usable. Use a dedicated RPC node and authenticated, rate-limited
ingress when exposing submission over TCP. The plugin only signs transactions
it builds with one configured proxy action. It does not accept an outer
transaction from the caller.

## Configuring routes

To override the built-in routes, copy [the example file](interop-routes.example.json)
to `interop-routes.json` beside `config.ini`. The node reads that fixed path in
its config directory at startup. Restart after changing it. If the file is
absent, the built-in routes still work. An invalid file stops startup. The
file replaces the entire route list.

Select `fullon-interop-v1` (the default) or `fullon-interop-v2` (for older
deployments) using `signing_protocol`. The configured version must match
`interopproxy::global.signing_protocol`. A missing chain field denotes v2.
The version selects the message prefix and leaves the field encoding fixed.
Each route names the
contract actions, ABI field mappings, expected binding signature type and
wallet signing method. The proxy contract must already support the same
source and protocol. After each WASM upgrade, call `interopproxy::syncroutes`
with the proxy account's authority. The action publishes the code-hash-bound
`routemap`. The plugin checks configured routes against the manifest and ABI,
and caches the validation by both hashes. The known legacy testnet WASM
has a fixed compatibility mapping.

Use `GET /v1/interop/get_routes` to discover the enabled operations, signature
type, wallet signing method, and required wallet fields per source. Bind
readiness reflects the owner subsidy policy and any required verifier pin.
Configure `interopowner::setbindrules` before allowing new accounts and
`interopproxy::pinverifier` before enabling TON. Retired bindings are rejected
for execution and still count as existing bindings. The top-level
`submission_enabled` reports whether this node is configured to relay.

## Prepare

`POST /v1/interop/prepare`

Bind request:

```json
{"operation":"bind","source":"eth","address":"0x..."}
```

Execute request:

```json
{
  "operation":"execute",
  "source":"solana",
  "address":"ExternalWalletAddress",
  "txns":[{"contract":"flon.token","action":"transfer","data":"001122"}]
}
```

Action data may be ABI-encoded hex or a JSON object. Preparation ABI-encodes
JSON using the current target ABI and returns the exact bytes in
`submit_args.txns`. The response contains
`signing_message`, `expires_at`, `chain_id`, `proxy_contract`,
`owner_contract`, `signing_protocol`, `wallet_signing`, and `submit_args`. An allocated name is a candidate until
the binding executes on chain. If it becomes unavailable, prepare again and
sign the new request.

## Submit

`POST /v1/interop/submit`

Send the fields from `submit_args` at the top level, adding `signature` and
`wallet_fields` as required by the route. Use the wallet-specific framing
required by the contract. The returned `signing_message` is the message
before wallet framing. Vector fields use hex and secp signatures use the
FullOn ABI signature string representation.

| Route | Bind wallet fields | Execute wallet fields |
| --- | --- | --- |
| `eth`, `bsc`, `arb`, `tron` | `msg_header`, `l1_pubkey`, `l2_pubkey` | `msg_header` |
| `btc` | `msg_header`, `l1_pubkey`, `l2_pubkey` | Unsupported by the contract |
| `solana` | None | None |
| `ton` | `msg_header1`, `msg_header2`, `l1_pubkey`, `l1_chain_id`, `l1_version`, `l1_revision`, `l1_bounce`, `l1_testnet` | Unsupported by the contract |
| `statter` | `l1_pubkey` | None |

The plugin validates the binding and nonce again before it signs and submits.
The contract verifies the wallet signature and dispatches the inline actions.
A successful response is HTTP 202 with `transaction_id`. Use chain transaction
status to verify inclusion and finality.

Identical in-flight submissions share one result, with up to eight waiters.
Binding or nonce conflicts return HTTP 409, expired requests return 410, and
a saturated relayer returns 429. The callback deadline leaves two seconds
before the HTTP response-generation deadline. After reservation, a timeout
returns HTTP 504 with the transaction ID. Check its status before retrying.

## Testing

With `BUILD_TESTS=ON`, run `ctest -R interop_pending_submissions_test` for
in-flight timeout and late-callback regression coverage.

Against a disposable or test node with TCP submit enabled, run:

```sh
python3 interop_rpc_smoke.py --url http://127.0.0.1:8888 --cases cases.json
```

Supply a JSON list of cases covering at least two signature types. Each case
contains an unbound `address`, a `source` with bind and execute support, a
`signer` command as a string array, and an `actions` array of test actions.
The script replaces exact `"$account"` values in actions with the new FullOn
account. The signer reads the complete prepare response on stdin and emits
`{"signature":"…","wallet_fields":{…}}` on stdout. Use controlled wallets and
actions on your test network. The script submits real binding and execution
transactions, and checks expiry, replay, and a wrong wallet signature. Supply
`--mismatch-url` for the optional protocol mismatch check.

For the transport timeout check, point `--timeout-url` at a second relayer on
the same test chain with a deliberately slow wallet signer. For example,
set `http-write-timeout-ms = 5000`, set `wallet-provider-timeout = 6000`,
and delay the test signer by more than three seconds. The script checks it
receives a JSON 504 with the transaction ID before the connection closes.
