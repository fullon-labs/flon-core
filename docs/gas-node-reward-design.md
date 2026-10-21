# FLON Gas 节点奖励设计与部署说明

每个 6 小时 epoch 将链上实际消耗 Gas 对应 FLON 的 10% 从 `flon.gas`
划入独立账户 `gas.reward`，由最多 21 个活跃生产节点和 30 个合格备用节点
分享。其余 90% 继续留在 `flon.gas`。Gas 是资源计量单位，不创建 ELON
作为中间奖励币。

## 共识收入计量

core 在 CPU/NET 和 RAM Gas 实际扣费成功时累计：

- `total_gas_consumed`
- `total_gas_value_flon`

`buygas` 只是预付，不计为消费；预付 Gas 真正使用时才累计。两个值进入链状态，
并由协议特性 `GAS_FEE_TOTALS` 激活后的 `get_gas_fee_totals` intrinsic 提供给
系统合约。当前协议换算为 1 Gas 原子单位对应 1 FLON 最小单位，但保留两个字段，
便于未来治理调整换算规则。

## 增量统计与结算

`flon.system::onblock` 在每个 epoch 首块冻结当期最多 21 个活跃节点和按票数排名的
30 个备用节点，并读取 `get_producer_slot_stats`，只累计上次 slot 到当前 slot 的
应出、实出和漏块；备用节点通过 `standbyproof` 持续证明在线。Gas totals 在 pending
block 开始时读取，因此增量归入前一个已完成 slot，避免 epoch 边界错记。epoch 结束后：

1. `beginsettle` 冻结该 epoch 的 Gas 增量和 10% 奖励池；
2. `settlenext(epoch, max_nodes)` 每次最多处理 10 个节点，第一遍判定资格并累计
   分母，第二遍按确定顺序计算分配；
3. 分配完成后 `finalizeepoch` 以 O(1) 完成状态确认；
4. `gas.reward` 保存 `rewardepochs` 和 `noderewards` 资金账本，节点调用
   `claimgas` 自行领取。

若合格活跃节点少于当期活跃集合的三分之二，结算进入 `incident_hold`，不会自动
转出资金。治理账户必须调用 `resolveincdt(epoch, true)` 明确继续，或以 `false`
取消当期奖励并让资金继续留在 `flon.gas`。

单块通常只更新当前排班涉及的节点；结算总工作量仍为 O(51)，但拆成上限明确的
多笔交易，避免单笔 `settleepoch` 扫描全部节点。

## 资格与确定性

- 活跃节点默认出勤率至少 95%；活跃子池的 40% 等分作为基础奖励，60% 按本期
  实出块数分配。
- 备用节点必须是已注册且非当前活跃排班的生产者，默认每期至少提交 12 次
  `standbyproof`；备用子池由合格节点等分。
- 账户按 `name.value` 固定顺序处理；整数除法余数由最后一个合格节点吸收，保证
  `sum(allocation) == funded`。
- 每期在 `flon.system` 保存原始统计、资格、score 和最终 allocation；
  `gas.reward` 保存累计 allocated/claimed/unclaimed。

## 上线顺序

1. 升级 core/CDT，并激活 `PRODUCER_SLOT_STATS` 和 `GAS_FEE_TOTALS`；
2. 创建 `gas.reward`，部署并 `init`；
3. 部署新版 `flon.system`；
4. 将 `flon.gas@active` 授权给 `flon@eosio.code`，仅用于 epoch 资金划转；
5. 调用 `setgasrwcfg(true, 43200, 9500, 12, 1000, 8000, 4000)`，即 Gas 收入
   最多 10% 进入节点池，其中活跃/备用子池为 80%/20%；某一子池无合格节点时，
   该子池不转出并继续留在 `flon.gas`；
6. 观察至少一个完整 epoch 后再开始结算。

协议特性未激活前不能启用配置，否则 `onblock` 调用新 intrinsic 会失败。升级新增
resource-limits 共识字段，旧节点必须统一升级并按发布说明完成 replay/snapshot 兼容
验证后再激活。
