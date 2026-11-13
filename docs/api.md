# API 接口说明

客户端提交游戏状态，服务端返回决策。默认地址为 `http://127.0.0.1:8081`，地址和端口可在服务配置中修改。

| 方法 | 路径 | 用途 |
| --- | --- | --- |
| `POST` | `/labyrinth/decision` | 请求决策 |
| `POST` | `/labyrinth/result-screenshot` | 上传通关结算截图 |

两个接口均使用 HTTP。示例中的数值和返回结果用于说明接口格式，实际决策取决于请求中的状态、策略配置和模型。

## 连接与请求头

| 请求头 | 要求 |
| --- | --- |
| `uid` | 必填且只能出现一次，填写正整数 ViewerId，范围为有符号 64 位整数 |
| `Content-Length` | 必填且只能出现一次，填写请求体的实际字节数；常见 HTTP 库和 curl 会自动设置 |
| `Content-Type` | 决策请求使用 `application/json`；截图上传必须严格为 `image/png` |

服务端目前每条连接只处理一次请求，客户端应能处理响应后的连接关闭。`uid` 用于标识日志中的帐号，不是认证凭据。多帐号请求分别携带对应的 `uid`。

## 公共请求字段

每个 `/labyrinth/decision` 请求都需提供表中的必填字段；空数组用 `[]` 表示。

| 字段 | 类型 | 必填 | 含义 |
| --- | --- | --- | --- |
| `protocol_version` | integer | 是 | 固定为 `1` |
| `request_id` | string | 是 | 非空请求标识，响应原样返回；建议每次请求使用不同值 |
| `kind` | string | 是 | 决策类型，见下文 |
| `enter_id` | integer | 是 | 本局标识；开局、寻路和刷分要求大于 `0` |
| `region` | integer | 是 | 当前区域编号；地图区域通常为 `1`～`5`，结算时按实际状态发送 |
| `block_id` | integer | 是 | 当前格子 ID；无适用位置时可为 `0`，寻路和刷分要求大于 `0` |
| `difficulty` | integer | 是 | 当前难度；开局、寻路、刷分和清票要求大于 `0` |
| `gold` | integer | 是 | 当前持有的阿尔法金币 |
| `owned_unit_ids` | integer[] | 是 | 本局已获得的完整角色 ID 列表 |
| `owned_relic_ids` | integer[] | 是 | 本局已获得的完整遗物 ID 列表 |
| `account_name` | string | 否 | 帐号昵称，用于日志显示 |

以上整数字段范围为 `0`～`2147483647`，ID 数组中的元素必须为正整数。要求整数的字段不能发送字符串或小数。昵称超过 128 个 UTF-8 字节、为空或含控制字符时，日志不采用该昵称，决策不受影响。

服务端根据本次请求携带的状态计算，不累计角色、遗物或失败历史。`request_id` 用于关联响应和日志，不提供幂等去重；重复发送仍会重新计算。

下文的请求示例只列出各类型的附加字段。发送时，需与公共字段合并到同一个 JSON 对象中，不使用 `payload` 包装层。

## 决策类型

| `kind` | 用途 | 可能的 `action` |
| --- | --- | --- |
| `select_ticket` | 开局选人、普通自选券 | `select`、`noop` |
| `unit_choice` | 角色三选一 | `select`、`noop` |
| `ticket_choice` | 印记选择 | `select`、`noop` |
| `relic_choice` | 遗物选择 | `select`、`noop` |
| `event` | 事件选择 | `select`、`noop` |
| `shop` | 商店购买与刷新 | `shop_plan`、`noop` |
| `team` | 编队建议及战斗重试 | `team`、`retry_run`、`manual`、`noop` |
| `opening` | 开局筛选 | `opening_accept`、`opening_retry`、`opening_stop`、`noop` |
| `path` | 寻路 | `move`、`noop` |
| `score_check` | 最终首领前的分数判断 | `fight`、`retry_run`、`noop` |
| `ticket_clear` | 结算后是否继续清票 | `continue_run`、`noop` |

### select_ticket：自选角色

| 附加字段 | 类型 | 必填 | 含义 |
| --- | --- | --- | --- |
| `ticket_id` | integer | 是 | 自选券 ID，大于 `0` |
| `select_count` | integer | 是 | 本次需要选择的人数，大于 `0` |
| `candidate_unit_ids` | integer[] | 是 | 当前券允许选择的全部角色 ID |

开局券和普通自选券共用接口，分别由各自的配置开关控制。开局券从配置的角色池中随机选出所需人数；普通券按配置的优先顺序选择。无法选足时返回 `noop`。

成功响应包含 `action: "select"` 和 `unit_ids`，见快速调用示例。

### unit_choice：角色三选一

```json
{
  "kind": "unit_choice",
  "choices": [
    {"choice_num": 1, "unit_id": 102501},
    {"choice_num": 2, "unit_id": 130801},
    {"choice_num": 3, "unit_id": 110301}
  ],
  "boss_quest_ids": [770331601, 770531601],
  "future_ex_regions": [3, 4, 5]
}
```

- `choices` 必填；每项包含正整数 `choice_num` 和 `unit_id`，`choice_num` 不得重复。当前角色评估要求恰好三个候选，且请求难度需与角色选择配置一致。
- `boss_quest_ids` 可选，最多十个正整数关卡 ID。建议提供本局第三、第五层实际首领的关卡 ID，用于评估后续目标；缺少首领关卡时，可能跳过对应的模型评分。
- `future_ex_regions` 可选，列出仍可能遭遇 EX 的区域，每项为 `1`～`5`，最多五项；省略时按 `1`～`5` 处理，计算时会排除早于当前区域的关卡。

评估时使用公共字段中的已有角色和遗物，响应返回选项序号：

```json
{"request_id": "demo-001", "action": "select", "choice_num": 2}
```

### ticket_choice：印记选择

```json
{
  "kind": "ticket_choice",
  "choices": [
    {"choice_num": 1, "ticket_id": 31100000, "count": 3},
    {"choice_num": 2, "ticket_id": 31200000, "count": 3},
    {"choice_num": 3, "ticket_id": 31500000, "count": 3}
  ]
}
```

`choices` 必填，每项的 `choice_num`、`ticket_id` 和 `count` 均为正整数，`choice_num` 不得重复。按配置的属性优先顺序选择，成功时返回 `select` 和 `choice_num`。

### relic_choice：遗物选择

```json
{
  "kind": "relic_choice",
  "choices": [
    {"choice_num": 1, "relic_id": 40001, "count": 1},
    {"choice_num": 2, "relic_id": 40003, "count": 1},
    {"choice_num": 3, "relic_id": 50003, "count": 1}
  ]
}
```

`choices` 必填，每项的 `choice_num`、`relic_id` 和 `count` 均为正整数，`choice_num` 不得重复。先比较星级，再比较配置的类别顺序等条件。成功时返回 `select` 和 `choice_num`。

`unit_choice`、`ticket_choice` 和 `relic_choice` 均可附带 `ticket_id`、`current_choice_count`、`remain_choice_count` 三个非负整数字段，用于记录当前券和选择进度。

### event：事件选择

| 附加字段 | 类型 | 必填 | 含义 |
| --- | --- | --- | --- |
| `event_id` | integer | 是 | 事件 ID，大于 `0` |
| `options` | object[] | 是 | 当前事件提供的选项 |

每个选项包含：

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `choice_num` | integer | 正整数选项序号，同一事件内不得重复 |
| `choice_id` | integer | 正整数选项 ID |
| `condition_type` | integer | 非负条件类型：`0` 无条件，`1` 检查金币 |
| `condition_value` | integer | 非负条件数值；金币条件要求 `gold >= condition_value` |
| `unit_id` | integer | 选项携带的角色 ID，没有则为 `0` |

按事件配置返回 `select` 和 `choice_num`。未配置自动选择、配置的选项不在请求中，或条件不满足时，返回 `noop`；其他条件类型的选项目前不会自动选择。

### shop：商店购买

```json
{
  "kind": "shop",
  "reset_count": 0,
  "refresh_cost": 300,
  "reset_limit": 2,
  "items": [
    {
      "lineup_id": 1,
      "reward_type": 25,
      "reward_id": 31001000,
      "count": 3,
      "price": 1200,
      "sold": 0,
      "availability": 1
    }
  ]
}
```

| 附加字段 | 类型 | 必填 | 含义 |
| --- | --- | --- | --- |
| `reset_count` | integer | 是 | 已刷新次数，非负整数 |
| `refresh_cost` | integer | 是 | 下一次刷新实际费用，非负整数 |
| `reset_limit` | integer | 是 | 游戏允许的刷新上限，非负整数；当前规划最多考虑两次刷新 |
| `items` | object[] | 是 | 当前页面的完整商品列表，最多九件 |

商品必须提供全部字段。`lineup_id`、`reward_id`、`count` 为正整数，`lineup_id` 不得重复；`reward_type`、`price`、`sold` 为非负整数，`availability` 为大于等于 `-1` 的整数。

`reward_type` 的 `25` 表示印记券、`26` 表示遗物。仅 `sold == 0` 且 `availability == 1` 的商品可购买，`price` 填写实际折扣价格。

```json
{
  "request_id": "demo-001",
  "action": "shop_plan",
  "buy_lineup_ids": [1],
  "after": "refresh"
}
```

先购买 `buy_lineup_ids` 中的商品，再执行 `after`：`refresh` 刷新，`close` 关闭。购买列表可以为空。购买或刷新后，应在下一次请求中发送最新金币和完整商品状态；响应只描述当前页面的操作。

### team：编队与重试

```json
{
  "kind": "team",
  "quest_id": 770312201,
  "party_count": 1,
  "past_attempts": [],
  "unit_npc_flags": [
    {"unit_id": 102501, "is_npc": false},
    {"unit_id": 130801, "is_npc": true}
  ]
}
```

| 附加字段 | 类型 | 必填 | 含义 |
| --- | --- | --- | --- |
| `quest_id` | integer | 是 | 正整数关卡 ID |
| `party_count` | integer | 是 | 普通、EX 填 `1`，首领填 `3` |
| `past_attempts` | object[] | 是 | 当前关卡此前的尝试，没有则为 `[]` |
| `unit_npc_flags` | object[] | 否 | 角色 NPC 标记；每项包含正整数 `unit_id` 和布尔值 `is_npc`，角色 ID 不得重复 |
| `existing_parties` | integer[][] | 否 | 当前界面已有编队，最多三队、每队最多五人；当前不参与候选排序 |
| `account_training` | object | 否 | 完整模拟验证所需的帐号加成，见下文；缺失时退回 Critic 推荐 |

当前自动编队支持难度 `5` 和模型收录的关卡。第三层首领请求仍填 `party_count: 3`，服务端按配置返回一、二或三队；第五层首领返回三队。多队方案中的角色不重复。开启忽略 NPC 时，只有剔除后仍至少有五名角色，才会使用非 NPC 角色池。

`past_attempts` 中每项的格式为：

```json
{
  "parties": [[102501, 130801, 110301, 107101, 107501]],
  "outcome": "defeat"
}
```

`parties` 最多三队、每队最多五个正整数角色 ID；`outcome` 为 `unknown`、`defeat` 或 `victory`。客户端每次提供当前关卡的完整历史，服务端据此统计失败次数、排除已失败方案。只有 `enter_id`、`block_id` 大于 `0`，且至少有一条 `defeat` 记录，才会触发战斗重试的后续处理。

编队成功：

```json
{
  "request_id": "demo-001",
  "action": "team",
  "parties": [[102501, 130801, 110301, 107101, 107501]]
}
```

已有失败记录时，若失败次数 **大于** 配置的重试次数，或没有新队伍可尝试，服务端会返回后续处理动作：

```json
{
  "request_id": "demo-001",
  "action": "manual",
  "defeat_count": 5,
  "max_retries": 4
}
```

`manual` 表示转人工；配置为重新开局时返回 `retry_run`。难度或关卡不受支持、首次无法组队等情况可能返回 `noop`。

#### 帐号加成 account_training

```json
{
  "account_training": {
    "talent_levels": [{"talent_id": 1, "level": 17}],
    "talent_skill_nodes": [{"node_id": 638, "enhance_level": 5}],
    "master_skill_node": {"node_id": 37, "enhance_level": 2},
    "role_mastery": [
      {"unit_role_id": 7, "slot_levels": [1, 2, 3, 4], "enhance_levels": [0, 1, 2, -1]}
    ],
    "labyrinth_enhanced_node_ids": [41, 7, 23]
  }
}
```

- `talent_levels`：属性等级，填写 `talent_id` 和 `level`。
- `talent_skill_nodes`：属性技能节点，填写 `node_id` 和 `enhance_level`。
- `master_skill_node`：大师技能节点，填写 `node_id` 和 `enhance_level`；没有时可省略这一项。
- `role_mastery`：职能练度，填写 `unit_role_id`，以及各含四项的 `slot_levels` 和 `enhance_levels`，按游戏数据原值发送。
- `labyrinth_enhanced_node_ids`：黎明界已强化节点 ID 列表。

大师节点可以省略；其余列表均需提供，无内容时发送 `[]`。模拟时，单角色按满练度处理；帐号属性、职能、大师和黎明界强化使用上述数据，遗物与金币从公共字段读取。不需要额外发送单角色装备或等级。

只有一套可用队伍时跳过模拟；连接失败、数据缺失或模拟未获胜时继续使用 Critic 推荐。预测胜率、模拟结果和推荐摘要作为诊断信息记录在决策日志中；HTTP 响应中的 `parties` 是最终执行方案，不附带这些分数。

### opening：开局筛选

| 附加字段 | 类型 | 必填 | 含义 |
| --- | --- | --- | --- |
| `attempt` | integer | 是 | 本次开局尝试序号，从 `1` 开始 |
| `map` | object[] | 是 | 本局地图，至少一项，最多 1024 项；需覆盖待检查的所有区域 |

难度 `1` 检查前三个区域，其余难度检查五个区域。地图块结构见下节。满足配置条件时返回：

```json
{"request_id": "demo-001", "action": "opening_accept", "max_attempts": 100}
```

不满足条件但仍可重试时返回：

```json
{"request_id": "demo-001", "action": "opening_retry", "max_attempts": 100, "rejected_region": 3}
```

不满足条件且达到上限时，`action` 为 `opening_stop`。`rejected_region` 是首个不满足要求的区域。客户端按 `action` 处理，`max_attempts` 用于展示上限；具体未满足的格子或首领条件记录在日志的诊断信息中。

### path：寻路

附加字段为 `map`，只发送当前区域的完整地图，最多 1024 项。公共字段中的 `region` 必须为 `1`～`5`，`block_id` 指向当前已完成的格子。

`opening` 和 `path` 的地图块字段如下：

| 字段 | 类型 | 必填 | 含义 |
| --- | --- | --- | --- |
| `block_id` | integer | 是 | 正整数格子 ID，同一区域内不得重复 |
| `region` | integer | 是 | 所属区域，`1`～`5` |
| `column` | integer | 是 | 正整数列号，从 `1` 开始 |
| `block_type` | integer | 是 | 格子类型，协议接受 `-1`～`8`，常用值见下表 |
| `next_block_ids` | integer[] | 是 | 后继格子 ID；没有则为 `[]`，不得重复 |
| `boss_unit_ids` | integer[] | 否 | 首领节点的敌方角色 ID，供首领名单筛选使用 |
| `cleared` | boolean | `path` 必填 | 该格子是否已完成；`opening` 不需要此字段 |

| `block_type` | `0` | `1` | `2` | `3` | `4` | `5` | `6` | `7` | `8` |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 含义 | 未知 | 起点 | 普通 | 极难 EX | 印记 | 事件 | 遗物 | 商店 | 首领 |

请求附加字段示例：

```json
{
  "kind": "path",
  "region": 3,
  "block_id": 30101,
  "map": [
    {"block_id": 30101, "region": 3, "column": 1, "block_type": 1, "next_block_ids": [30201], "cleared": true},
    {"block_id": 30201, "region": 3, "column": 2, "block_type": 8, "next_block_ids": [], "boss_unit_ids": [301206], "cleared": false}
  ]
}
```

找到符合配置的路径时返回：

```json
{"request_id": "demo-001", "action": "move", "next_block_id": 30201}
```

已经到终点、位置状态不适用或无符合路径时返回 `noop`。无符合路径的原因保存在日志中，不附加到响应里。

### score_check：最终首领前的分数判断

```json
{
  "kind": "score_check",
  "region": 5,
  "block_id": 50801,
  "guild_id": 3,
  "quest_id": 770532001,
  "unit_count": 50,
  "relic_count": 26,
  "normal_clear_count": 6,
  "ex_clear_count": 6,
  "boss_clear_count": 1,
  "areas": [
    {"region": 1, "block_count": 5, "cleared": true},
    {"region": 2, "block_count": 6, "cleared": true},
    {"region": 3, "block_count": 6, "cleared": true},
    {"region": 4, "block_count": 7, "cleared": true},
    {"region": 5, "block_count": 6, "cleared": false}
  ]
}
```

在最终首领战斗前提交，附加字段全部必填：

- `guild_id`、`quest_id` 分别为公会 ID 和首领关卡 ID，均为正整数。
- `unit_count`、`relic_count` 为当前角色数和遗物数，服务端直接使用这两个统计值计分。
- `normal_clear_count`、`ex_clear_count`、`boss_clear_count` 为已经通关的对应战斗次数。
- `areas` 从区域 `1` 列到当前区域，每个区域一项，不得重复。每项包含正整数 `region`、当前已踏破格子数 `block_count` 和布尔值 `cleared`。
- 上述五个计数和每个区域的 `block_count` 均为 `0`～`10000` 的整数。

难度 `1` 的最终区域为 `3`，其余为 `5`。之前的区域应为 `cleared: true`，最终区域为 `false`。倍率从游戏数据按 `difficulty` 读取，客户端不必发送倍率。

**按当前实际进度发送，客户端不要预先加上最后一战。** 服务端会计入最后首领的通关次数、最后一个格子及区域完成分。例如已经击败一只首领时发送 `boss_clear_count: 1`，服务端预测最终通关分时按两只计算。

```json
{"request_id": "demo-001", "action": "retry_run", "score": 50800, "minimum_score": 52000}
```

`score` 是预计通关分，`minimum_score` 是当前公会目标。满足 `score >= minimum_score` 时返回 `fight`，否则返回 `retry_run`。未配置该公会目标时，目标按 `0` 处理。策略未启用、当前不是最终区域首领或状态不适用时返回 `noop`。

### ticket_clear：结算后继续清票

```json
{"kind": "ticket_clear", "guild_id": 3, "ticket_count": 1}
```

`guild_id` 填写当前公会 ID，必须为正整数；`ticket_count` 填写当前剩余票数。两个字段均必填。公共字段仍需提供，其中 `difficulty` 必须大于 `0`。

剩余票数大于配置的保留票数，且有可用目标公会时返回：

```json
{"request_id": "demo-001", "action": "continue_run", "guild_id": 3}
```

`guild_id` 是下一局公会，按配置名单轮换；响应不指定难度。剩余票数未超过保留票数，或没有可用目标公会时，返回 `noop`。

## noop 与动作处理

```json
{"request_id": "demo-001", "action": "noop"}
```

`noop` 是 HTTP 200 的正常响应，表示本次不执行自动操作。总开关关闭、对应策略未启用或当前状态不适用时，都可能返回这一响应。客户端收到后应结束本次自动处理，不能继续执行上一条决策。

`select` 响应包含 `choice_num` 时，按选项序号处理；包含 `unit_ids` 时，按角色列表处理。`retry_run` 建议撤退后重新开局；`opening_retry` 表示开局筛选重试；`continue_run` 表示结算后继续下一局。具体界面操作由客户端负责。

总开关只控制决策，截图上传不受影响。无效请求仍返回协议错误，不会变成 `noop`。

## 结算截图上传

请求体为 PNG 原始字节，元数据通过 URL 查询参数提供，每个参数只能出现一次。

| 参数 | 类型 | 含义 |
| --- | --- | --- |
| `guild_id` | integer | 游戏数据中存在的公会 ID |
| `unit_count` | integer | 当前角色数，非负整数 |
| `relic_count` | integer | 当前遗物数，非负整数 |
| `score` | integer | 实际结算分，非负整数 |

```bash
curl -X POST \
  'http://127.0.0.1:8081/labyrinth/result-screenshot?guild_id=3&unit_count=50&relic_count=26&score=50800' \
  -H 'Content-Type: image/png' \
  -H 'uid: 123456789' \
  --data-binary @result.png
```

成功返回 HTTP **201**：

```json
{"filename": "咲恋救济院_角色50_遗物26_50800分_2026-11-14T12-00-00.000Z.png"}
```

文件保存在当前配置文件旁的 `screenshots/` 目录。文件名包含公会名、角色数、遗物数、实际分数和服务端 UTC 时间；重名时追加编号。此接口不需要 JSON 公共字段或 `request_id`，也不依赖清票决策是否为 `continue_run`。

## 错误响应

| HTTP 状态 | 含义 |
| --- | --- |
| `200` | 决策成功，包括 `noop` |
| `201` | 截图保存成功 |
| `400` | 请求头、JSON、必填字段、取值或截图元数据等不合法 |
| `404` | 请求路径不存在 |
| `415` | 截图的 `Content-Type` 不是 `image/png` |
| `500` | 决策内部异常或截图保存失败 |

协议错误示例：

```json
{"error": "unknown kind"}
```

请求解析成功后，决策计算异常的响应示例：

```json
{"request_id": "demo-001", "error": "Internal decision error"}
```

错误响应不保证包含 `request_id`，也没有可执行的 `action`。先检查 HTTP 状态，再读取 JSON；底层 HTTP 错误（例如请求体超限的 `413`）可能不是 JSON。重试前应获取最新状态，避免重复执行已完成的购买、移动或选择操作。

## 实现参考

- [请求解析](../src/protocol.cpp)
- [HTTP 与截图接口](../src/server.cpp)
- [决策分发](../src/decisions.cpp)
- [模拟器数据转换](../src/simulator.cpp)
