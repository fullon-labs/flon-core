---
content_title: interop_relayer_api_plugin
---

`interop_relayer_api_plugin` 为外部钱包提供 FullOn RPC。钱包用原有私钥签署绑定或执行请求，节点用中继账号签署外层 FullOn 交易。链上的 `interopproxy` 验证钱包签名，验证成功后由 `interopowner` 执行请求中的 inline actions。

## 启动节点

使用编入此插件的 `funod`。目标链上的代理合约须支持所选签名协议，`interopproxy::global.chain_id` 须与节点 `get_info` 返回的 `chain_id` 一致。中继账号须在该代理合约中处于有效的 miner 状态，并拥有调用相应 `bind*` 和 `push*` action 的权限。建议使用独立的中继子权限，并将其链接到需要开放的代理合约 actions。

在现有 RPC 节点的 `config.ini` 中加入以下配置。将账号、权限和签名提供方的占位值替换成部署的实际值。

```ini
plugin = eosio::chain_api_plugin
plugin = eosio::interop_relayer_api_plugin
http-server-address = 127.0.0.1:8888
unix-socket-path = /run/fullon/interop.sock
api-accept-transactions = true

interop-contract = interopproxy
interop-relayer-permission = relayminer@relay
interop-signature-provider = <RELAYER_PUBLIC_KEY>=WALLET:unix:///run/fullon/wallet.sock/v1/wallet/sign_digest
wallet-provider-timeout = 1000
interop-signing-threads = 2
interop-max-in-flight = 32
```

`WALLET:` 后须填写完整的签名端点，包含 `/v1/wallet/sign_digest`。例如上述 Unix socket 地址，也可使用 `https://wallet.example/v1/wallet/sign_digest`。`wallet-provider-timeout` 以毫秒计，须在 1 至 30000 之间。

启动前，确保 socket 的父目录存在且运行 `funod` 的用户可以写入，签名提供方也可以访问对应的中继密钥。然后使用原有数据目录和网络配置启动或重启节点：

```sh
funod --config-dir /etc/fullon --data-dir /var/lib/fullon
```

`interop-contract` 默认是 `interopproxy`。同时省略两个中继签名选项时，节点仅提供准备功能。配置提交功能的节点需接受 API 交易，不能使用 `read-mode = irreversible`。

默认情况下，`prepare` 可通过 TCP 调用，`submit` 只通过 Unix socket 调用。若部署在受认证且带限流的网关后面，并需要由网关通过 TCP 转交提交请求，可设置 `interop-submit-tcp = true`。远程客户端不需要访问钱包签名服务。

## 发布并发现链上路由

部署或升级 `interopproxy` WASM 和 ABI 后，由合约账号运行：

```sh
fucli -u "$NODE_URL" push action interopproxy syncroutes '[]' -p interopproxy@active
```

`syncroutes` 将该 WASM 实际支持的 `source`、签名类型和 action 写入
`routemap`，并记录当前 WASM 哈希。节点只接受与链上清单及 ABI 都匹配的路由。
已核验的旧测试链 v2 WASM 可继续使用原内置映射，其他部署需要发布清单。
升级 WASM 后要重新发布。节点按 ABI 和 WASM 哈希使路由校验缓存失效。

启用新账号绑定前，由 owner 合约账号调用 `interopowner::setbindrules`，
设置是否暂停和每日 FLON 补贴上限。补贴额度按链上 UTC 日期重置。
启用 TON 绑定前，由代理合约账号使用 `interopproxy::pinverifier`
批准当前 TON 验证合约的 WASM 哈希。更换验证合约代码后需更新批准。
已核验的旧测试链合约保留原有兼容行为。

钱包可先发现此节点开放的路由：

```sh
curl --fail-with-body -sS http://127.0.0.1:8888/v1/interop/get_routes | jq .
```

响应列出目标链和合约、签名协议，以及每条启用路由的签名类型、
钱包签名方法和各操作的必需辅助字段。操作值为 `null` 时，表示该操作不受支持。
`ready: false` 表示操作受支持但当前链上条件不满足，`readiness_code` 会给出原因。
`submission_enabled` 表示节点配置允许中继提交，TCP 或 Unix socket 仍由监听配置决定。
执行还需钱包地址拥有 active 绑定，已退役绑定返回 `409 binding_inactive`，原地址也不能再次绑定。

## 配置外部钱包路由

插件启动时读取 **与 `config.ini` 同目录**的 `interop-routes.json`。
例如使用 `--config-dir /etc/fullon` 时，路径是
`/etc/fullon/interop-routes.json`，无需在 `config.ini` 设置路径。

将 [完整配置示例](../../../../plugins/interop_relayer_api_plugin/interop-routes.example.json)
复制到配置目录并改名为 `interop-routes.json`。该文件完整替换内置路由，
因此只保留希望开放的 `source`。没有这个文件时使用内置路由。
配置不合法时节点启动失败。修改文件后重启节点生效。若使用 `--config` 指定了其他配置文件，则以该文件实际所在目录为准。

- `schema_version` 固定为 `1`。新部署建议使用默认的 `"signing_protocol": "fullon-interop-v1"`。旧部署若仍验 v2 前缀，可配置 `"fullon-interop-v2"`。
- 每个路由声明 `signature_type`（`secp`、`ed` 或 `rsa`）、`wallet_signing` 和对应 action。
  没有执行入口时设置 `"execute": null`。
- `prepare` 返回 `wallet_signing`，供客户端选择对应的钱包签名方法。
  `prepare` 和 `submit` 都会核对当前代理合约 ABI 中的 action 及参数。
- 新增 `source` 前，确保代理合约已用同一个值生成待验签消息，
  并且中继权限已链接到新增 action。路由文件不能改变消息字段编码或增加合约验签算法。

### 切换签名协议

v1 和 v2 使用相同的字段顺序，待签消息前缀分别是
`FullOn Interop v1:` 和 `FullOn Interop v2:`，签名不能互用。
合约首次执行 `setchainid` 默认启用 v1。已配置 chain ID 的旧合约升级后，
若 `global.signing_protocol` 尚未设置，仍使用 v2。

要将旧部署切换到 v1，先升级代理合约，由其账号授权调用
`setprotocol`（参数 `["v1"]`），再将节点配置改为
`"signing_protocol": "fullon-interop-v1"` 并重启。
节点发现 JSON 与链上协议不一致会返回 HTTP 503。
切换协议后，未提交的请求需要重新准备并签名。

## 准备并签署

统一使用 `POST /v1/interop/prepare`。用 `operation` 显式指定 `bind` 或 `execute`，用 `source` 指定外部钱包的签名路由。

绑定示例。先将 `WALLET_ADDRESS` 设置为实际的外部钱包地址：

```sh
jq -n --arg address "$WALLET_ADDRESS" \
  '{operation:"bind",source:"eth",address:$address}' |
  curl --fail-with-body -sS http://127.0.0.1:8888/v1/interop/prepare \
    -H 'Content-Type: application/json' --data-binary @- > prepared.json
```

执行已绑定账号的操作时，将同一个外部地址填入以下请求，并替换为实际要执行的动作。将请求保存为 `execute.json`：

```json
{
  "operation": "execute",
  "source": "eth",
  "address": "0x...",
  "txns": [{
    "contract": "flon.token",
    "action": "transfer",
    "data": {
      "from": "boundaccount",
      "to": "recipient",
      "quantity": "1.00000000 FLON",
      "memo": ""
    }
  }]
}
```

```sh
curl --fail-with-body -sS http://127.0.0.1:8888/v1/interop/prepare \
  -H 'Content-Type: application/json' --data-binary @execute.json > prepared.json
```

响应包含 `signing_message`、`expires_at` 和 `submit_args`，也列出目标 FullOn 链 ID、合约账号、`signing_protocol` 和 `wallet_signing`。绑定时，`submit_args.account` 是待分配的账号。执行时，插件读取当前绑定和 nonce，按 ABI 将 JSON 动作编码成十六进制字节，也接受已经编码的十六进制数据。

先在钱包核对目标网络、分配账号或要执行的动作，再用对应钱包签署 `signing_message`。签名格式遵循路由的规则。例如 ETH 使用 personal signing，提交时提供签名对应的消息前缀。钱包返回的 secp 签名需要转换成 FullOn ABI 接受的签名字符串。

## 提交并查询结果

将签名放入 `wallet-signature.txt`，将当前路由所需的签名辅助字段保存为 `wallet-fields.json`，然后将它们与原样返回的 `submit_args` 合并：

```sh
jq --rawfile sig wallet-signature.txt --slurpfile fields wallet-fields.json \
  '.submit_args + {signature: ($sig | rtrimstr("\n")), wallet_fields: $fields[0]}' \
  prepared.json > submit.json

curl --fail-with-body -sS --unix-socket /run/fullon/interop.sock \
  http://localhost/v1/interop/submit \
  -H 'Content-Type: application/json' --data-binary @submit.json
```

| `source` | 绑定所需的 `wallet_fields` | 执行所需的 `wallet_fields` |
| --- | --- | --- |
| `eth`、`bsc`、`arb`、`tron` | `msg_header`、`l1_pubkey`、`l2_pubkey` | `msg_header` |
| `btc` | `msg_header`、`l1_pubkey`、`l2_pubkey` | 合约未提供执行 action |
| `solana` | 无 | 无 |
| `ton` | `msg_header1`、`msg_header2`、`l1_pubkey`、`l1_chain_id`、`l1_version`、`l1_revision`、`l1_bounce`、`l1_testnet` | 合约未提供执行 action |
| `statter` | `l1_pubkey` | 无 |

字节向量字段使用十六进制。没有辅助字段时，`wallet-fields.json` 可写成 `{}`。成功提交返回 HTTP 202 和 `transaction_id`，可用交易状态 API 或区块浏览器确认收录和不可逆状态。若签名过期、绑定或 nonce 已变化，或分配的名字已被占用，应重新调用 `prepare` 并重新签名。

### 提交状态和限额

签名使用独立线程池。默认启用 2 个签名线程（可配置 1–8），最多保留 32 个在途提交（可配置 1–256），包括正在签名、排队和等待节点返回的请求。在同一个 RPC 节点上，相同请求共享一次签名和提交结果，最多 8 个调用方可同时等待该结果。该机制只覆盖在途请求，链上仍使用 nonce 防止重复执行。

| HTTP 状态 | 客户端处理 |
| --- | --- |
| 400 | 检查缺失或格式错误的字段、ABI 参数，以及路由是否与绑定的签名类型兼容。 |
| 409 | 绑定状态已变化，或另一个请求正在使用同一地址和 nonce。查询链上状态，再决定是否重新准备。 |
| 410 | 签名或外层提交期限已过。重新准备并签名。 |
| 429 | 中继已满或相同请求的等待者过多。退避后在签名仍有效时重试。 |
| 503 | 检查中继是否启用，以及签名协议、ABI 和链上路由清单是否匹配。 |
| 504 | 中继等待时间受 HTTP 响应和外层交易截止时间共同限制，至少为 HTTP 响应预留 2 秒。若响应包含 `transaction_id`，先查询该交易再决定是否重试。若返回 `request_deadline`，此次请求未提交交易。 |

## 回归测试

启用 `BUILD_TESTS` 后可运行 `ctest -R interop_pending_submissions_test`，
覆盖去重、争用、超时释放和迟到回调隔离。需要在测试节点验证钱包绑定与执行时，
使用 [RPC smoke 脚本](../../../../plugins/interop_relayer_api_plugin/interop_rpc_smoke.py)。
输入格式和签名适配器见 [插件 README](../../../../plugins/interop_relayer_api_plugin/README.md)。
