# WoL ESP32-C3 — v3 服务端架构(自建 EMQX + 后端中转)

> 状态:设计定稿,待实现
> 适用:面向终端用户的 v3 形态(BLE 配网 + 微信小程序)
> 关联文档:`BLE_REDESIGN.md`(配网 GATT 协议)、`../PROTOCOL.md`(MQTT 报文)

## 1. 背景与定位

早期设想"无服务端、设备直连 EMQX Serverless"。v3 改为**自建云服务器跑 EMQX**,
配套一个后端服务,配网与控制通过**微信小程序**完成。

核心决策:**小程序不直连 MQTT**,改为"后端常驻 MQTT 消费者 + 小程序走 HTTPS/SSE"。
理由(详见决策记录一节):

- 小程序无法常驻后台,直连只在前台有效,离线通知无论如何要走后端;
- 后端已经是必选项(claim 绑定、凭据签发、微信订阅消息),让它顺手做指令中转
  边际成本很小,却让客户端和 MQTT ACL 大幅简化;
- WoL 是请求-响应型交互,无持续数据流,后端中转无体感劣势。

## 2. 整体架构

```
                            ┌─────────────────────────────────────┐
                            │            云服务器                   │
  微信小程序                 │  ┌──────────┐      ┌──────────────┐  │
  ┌─────────┐   HTTPS       │  │  后端服务  │      │    EMQX       │  │
  │  小程序  │ ───────────▶ │  │ (API +    │◀────▶│  (broker)    │  │
  │         │ ◀─SSE/轮询──  │  │  MQTT     │ REST │              │  │
  │         │               │  │  消费者)   │ pub  │              │  │
  └────┬────┘   微信订阅消息  │  └────┬─────┘      └──────┬───────┘  │
       │      ◀───────────── │       │ HTTP auth         │          │
       │ BLE 配网            │       │ 事件消费(sub)      │ MQTT/TLS  │
       │                     └───────┼───────────────────┼──────────┘
       │                             │                    │
       ▼                             ▼                    ▼
  ┌─────────┐                   ownership 表          ┌─────────┐
  │  设备    │ ◀── BLE ─────────  设备状态缓存          │  设备    │
  │ ESP32-C3│      配网                                │ (MQTT)  │
  └─────────┘                                          └─────────┘
       └────────────────── WiFi + MQTT/TLS ──────────────────┘
```

### 组件职责

| 组件 | 职责 |
|------|------|
| **设备** | 连 EMQX;订自己的 cmd、发自己的 event。MQTT 凭据由后端签发,经 BLE 写入 |
| **EMQX** | broker;认证委托后端 HTTP;授权用模板(设备)+ 后端高权限账号(中转) |
| **后端** | ①小程序 API ②EMQX HTTP 认证端点 ③常驻 MQTT 消费者订 `home/wol/+/event` ④EMQX REST 发布指令 ⑤微信订阅消息推送 ⑥ownership/凭据/设备状态存储 |
| **小程序** | 纯 HTTPS 调后端;BLE 配网;不接触 MQTT |

## 3. Topic 与权限设计

Topic 沿用现有结构,不变:

```
home/wol/{device_id}/cmd     ← 指令(下行)
home/wol/{device_id}/event   → 事件(上行)
```

`device_id` = 设备 `mqtt_id` = `wol-<12位 MAC>`。

### 权限矩阵

| 主体 | clientid | publish | subscribe |
|------|----------|---------|-----------|
| **设备** | `wol-<mac>` | `home/wol/<自身>/event` | `home/wol/<自身>/cmd` |
| **后端中转** | `esp_auth_01`(username `esp_admin`) | `home/wol/+/cmd` | `home/wol/+/event` |
| 其它 | — | deny | deny |

小程序不在此表中(不连 MQTT)。设备被锁死在自身 topic;后端是唯一的"应用侧"
MQTT 主体,持通配权限。

### EMQX 授权配置

默认拒绝:

```
authorization.no_match = deny
authorization.deny_action = disconnect
```

授权链(按序匹配,命中即止,未命中走默认 deny):

```erlang
%% 1) 设备模板:零后端查询,所有设备共用一条
{allow, all, subscribe, ["home/wol/${clientid}/cmd"]}.
{allow, all, publish,   ["home/wol/${clientid}/event"]}.

%% 2) 后端中转账号(生产实际走 superuser——认证响应携带 is_superuser,
%%    EMQX 对超级用户跳过 ACL;下面两条 username 规则仅作双保险)
{allow, {username, "esp_admin"}, subscribe, ["home/wol/+/event"]}.
{allow, {username, "esp_admin"}, publish,   ["home/wol/+/cmd"]}.
```

> 设备授权用 `${clientid}` 占位符,EMQX 本地即可判定,后端不参与设备授权,
> 省去高频重连的后端压力。

## 4. 认证(EMQX HTTP authenticator)

EMQX 把认证委托给后端。请求体模板:

```json
{ "clientid": "${clientid}", "username": "${username}", "password": "${password}" }
```

后端按主体类型返回:

**设备**(`clientid` 以 `wol-` 开头):用 `clientid` 查 `devices` 表,比对 `password`
(=后端签发的 per-device 随机密码)与 `mqtt_pass_hash`:

```json
{ "result": "allow" }
```

**后端中转账号**(username `esp_admin`,固定强密码):

```json
{ "result": "allow", "is_superuser": true }
```

`is_superuser` 使 EMQX 对该连接跳过 ACL;第 3 节的 username 规则仅作双保险。
账号不绑定 clientid,可多端并发(如 MQTTX 调试),各连接独立认证、各自拿到
superuser 标记;注意调试端 clientid 勿与常驻消费者 `esp_auth_01` 相同(会互踢),
也勿以 `wol-` 开头(会被当设备拒掉)。

> 授权全部走第 3 节的 EMQX 本地规则,认证响应不需要再返回 `acl` 字段
> (因为没有"每用户动态 ACL"了——小程序不连 MQTT)。

## 5. 设备身份、凭据与归属模型

本节有两个**互相独立的层**,不要混:

- **归属层(谁是主人)**:简化为「**配网即重绑**」——见 5.1。
- **设备认证层(谁能连 broker)**:仍用后端签发的 `mqtt_pass`——见 5.2。

### 5.1 归属:配网即重绑(简化模型)

威胁模型前提:**能完成 BLE 配网 ≈ 物理在场 + 能读二维码 ≈ 等同持有设备**。对持有
设备的人做防护是徒劳,因此**不为所有权转移设任何密码学门槛**:

> **谁完成配网,后端就把该设备的 binding 无条件改成当前 App 用户。**

- `claim` 接口 = **覆盖式重绑**,不校验旧主人、不需要旧主人先解绑、不需要重置计数器。
- 3s / 10s 退化为**纯技术含义**(3s 保留配置改 WiFi 方便、10s 清空),**与归属无关**。
- 唯一真正该做的、且很便宜的产品防护:**二维码(PSK)放在外人路过拍不到的位置**
  ——设备**底部 / 内侧 / 电池仓 / 说明书**,而非正面外壳。把"在场"门槛从"看一眼"
  提到"上手翻一下"。

> 注:唯一不需要字面"动手碰"的配网入口是「WiFi 连不上自动进配网」,它把"物理在场"
> 放宽成"BLE 近场 + 能读二维码"。在本威胁模型下仍算在场,可接受。

### 5.2 设备认证:仍需 mqtt_pass(另一层)

归属简化**不等于**取消设备↔broker 的认证。`mqtt_pass` 解决的是另一类问题:防止别人
拿公开的 `mqtt_id` 冒充设备连 broker(发假事件、订 cmd、clientid 碰撞踢线),详见
第 4 节与「ID 白名单不能替代密码」决策。

| 数据 | 来源 | 是否公开 | 用途 |
|------|------|---------|------|
| `mqtt_id` | 芯片 eFuse MAC 派生 | 公开(二维码) | MQTT clientid + topic 标识 |
| `PSK` | 设备首次开机随机生成 | 公开(二维码) | BLE 配网通道加密 |
| `mqtt_pass` | **后端随机生成** | **私密** | MQTT 认证;仅经加密 BLE 下发到真设备 |

- `mqtt_pass` 是高熵随机串,**不从 mqtt_id/MAC/PSK 派生**,只走 PSK 加密的 BLE
  `config` 通道进真设备;后端只存其**哈希**(HMAC-SHA256 + pepper),**配网时轮换**。
- 配网这一步**同时**完成「重绑 + 签发/轮换 mqtt_pass」,两层在同一动作里一并搞定。

### 5.3 claim 接口的滥用防护(防枚举 + 限流)

`mqtt_id` 由芯片 MAC 派生、**同批次常连号**,看到一台就能猜邻居。PSK 虽在二维码上
(物理在场才拿得到),但若 claim 据响应差异区分「设备不存在 / 已停用 / PSK 不符」,
攻击者无需 PSK 也能遍历 mqtt_id 段、**探出哪些 ID 是真实出厂设备**(泄露铺货量/真实
ID)。故后端实现上:

- **泛化错误**:三类失败一律返回同一个 `403 claim_failed`(状态码与文案都不露差异),
  真实原因只记服务端日志。
- **令牌桶限流**:按 openid 计桶(容量 8 / 每 5 秒回填 1),挡高速枚举与 PSK 暴力。
  注:未配微信时 openid 来自 dev 桩可伪造,生产配 `code2session` 后该面收敛。

## 6. 时序

### 6.1 配网 + claim 绑定(配网即重绑)

```
用户        小程序            后端              EMQX        设备
 │  扫码      │                │                 │           │
 │─(mqtt_id, │                │                 │           │
 │   PSK)──▶ │                │                 │           │
 │           │─claim 请求─────▶│                 │           │
 │           │ (mqtt_id,PSK,   │ 查 devices 验真  │           │
 │           │  微信 openid)    │ binding=本用户   │           │
 │           │                │ (覆盖式,不校验旧主)│          │
 │           │                │ 生成/轮换 mqtt_pass│          │
 │           │◀─wifi 列表入口──│ (随机)           │           │
 │           │   + mqtt 配置    │                 │           │
 │           │  (含 mqtt_pass)  │                 │           │
 │           │────────────── BLE config(PSK 加密)────────────▶│
 │           │                │                 │   写入 LittleFS│
 │           │                │                 │   重启       │
 │           │                │                 │◀─连接(TLS)──│
 │           │                │   HTTP auth      │           │
 │           │                │◀────────────────│           │
 │           │                │──allow──────────▶│           │
 │           │                │                 │  online    │
 │           │                │◀── event(online,retain)──────│
 │           │  设备在线        │ 更新状态缓存      │           │
 │           │◀─SSE/轮询───────│                 │           │
```

`claim` 是**覆盖式重绑**:不校验旧主人、不需重置计数器(见 5.1 威胁模型)。
`devices` 表的 psk 校验只用于**验真**(确认是我们出厂的机器、mqtt_id 非伪造),
不用于"验主人"。防盗绑靠**二维码放置位置**这一物理措施(见 5.1)。

### 6.2 唤醒(下行指令)

```
小程序            后端                  EMQX            设备
 │  POST /devices/{id}/wol             │               │
 │──{mac?}──────▶│                     │               │
 │               │ 校验 openid 拥有该设备│               │
 │               │─REST publish────────▶│               │
 │               │  home/wol/{id}/cmd   │──{cmd:wol}───▶│
 │               │  {cmd:wol,mac}       │               │ 发魔法包
 │               │                     │◀─event(wol)───│
 │               │◀─(MQTT 消费者收到)────│               │
 │◀─SSE 推送结果──│ 关联 request          │               │
 │  或 2~3s 轮询  │                     │               │
```

后端用 EMQX REST API(`POST /api/v5/publish`)发布指令,用常驻消费者收 event,
按设备 + 时间窗关联到本次请求,回给小程序。

### 6.3 离线推送(App 关闭时)

```
设备            EMQX              后端                 微信         用户
 │  断网/掉线     │                 │                   │           │
 │──LWT offline─▶│                 │                   │           │
 │  (retain)      │── event ───────▶│ 消费者收到          │           │
 │               │                 │ 状态置离线           │           │
 │               │                 │ 查该设备订阅用户      │           │
 │               │                 │──订阅消息(模板)─────▶│──通知────▶│
```

需提前在小程序内引导用户授权"一次性订阅消息";唤醒成功、设备上下线等关键事件
由后端主动推。

## 7. 后端接口骨架

### 对小程序(HTTPS)

| 方法 | 路径 | 说明 |
|------|------|------|
| POST | `/auth/wechat-login` | 微信登录,换 openid + 会话 token |
| POST | `/devices/claim` | 扫码绑定(**覆盖式重绑**):body `{mqtt_id, psk, ...}`;验真后绑给当前用户、轮换 mqtt_pass、返回 BLE 下发所需的 mqtt 配置。失败泛化 + 限流见 5.3 |
| GET | `/devices` | 当前用户名下设备列表 + 在线状态 |
| POST | `/devices/{id}/wol` | 唤醒:body `{mac?}`;同步等待或返回 task id |
| POST | `/devices/{id}/cmd` | 透传其它指令(reboot/info/led/ota…),后端校验归属 |
| GET | `/devices/{id}/events` | SSE:前台打开时推该设备事件 |
| POST | `/devices/{id}/unbind` | 解绑 |

> **鉴权**:除 `/auth/wechat-login` 外,登录后所有接口均走两层——认证用 `Authorization:
> Bearer <token>`(登录签发的自包含 HMAC 会话 token,载 openid),授权再按 openid 校
> 设备归属(`ownerCheck`),非主人回 `403 not_owner`。

### 对 EMQX

| 方向 | 机制 |
|------|------|
| 认证 | EMQX → 后端 `POST /mqtt/auth`(第 4 节) |
| 发布指令 | 后端 → EMQX REST `POST /api/v5/publish` |
| 消费事件 | 后端常驻 MQTT client 订 `home/wol/+/event`;或用 EMQX 规则引擎 webhook 把 event POST 给后端 |

> 事件消费二选一:常驻 MQTT 订阅实现简单、实时;规则引擎 webhook 无需常驻连接、
> 更易水平扩展。小规模用常驻订阅即可。

### 对运维(管理后台 `/admin`)

内嵌的可视化设备管理后台(React 构建产物经 `//go:embed` 打进二进制),与小程序用户态
**完全分离**:设 `ADMIN_PASSWORD` 即启用,口令登录后走 HttpOnly cookie 会话。能力含
仪表盘、设备列表(状态过滤 + 搜索)、详情(归属/唤醒目标/启用停用/强制解绑/下发指令/
SSE 实时事件)、CSV 批量导入与单台补录、用户列表。详见 esp_auth 仓 `README.md`。

## 8. 存储(后端)

### 8.1 两种 MAC,先厘清

| 名字 | 是什么 | 落在哪张表 |
|------|--------|-----------|
| **芯片 MAC** | ESP32 eFuse MAC,派生出 `mqtt_id`(`wol-<12hex>`) | `devices`(设备身份) |
| **目标电脑 MAC** | 要被唤醒的 PC 网卡 MAC,魔法包目标 | `wake_targets`(用户管理) |

设备自身**不存**目标列表;固件按指令随包带 `mac`(见 PROTOCOL.md),目标是纯后端/App 侧数据。

### 8.2 实体关系

```
users(openid) 1───* bindings *───1 devices(mqtt_id)   ← 出厂登记表
                       │1
                       *
                  wake_targets   ← (binding, 电脑 MAC) 关联
```

### 8.3 表结构(SQLite,DDL 即落地)

```sql
-- 1. 出厂设备登记（产线批量导入，用户绑定前就存在）
CREATE TABLE devices (
  mqtt_id        TEXT PRIMARY KEY,          -- wol-<12hex>，= MQTT clientid
  chip_mac       TEXT NOT NULL,             -- eFuse MAC，与 mqtt_id 同源，便于检索
  psk            TEXT NOT NULL,             -- BLE PSK(32hex)，claim 校验设备真伪
  product_model  TEXT NOT NULL DEFAULT 'W1',
  batch          TEXT,                      -- 生产批次，可空
  mqtt_pass_hash TEXT,                      -- 后端签发的设备 MQTT 密码哈希；未配网为 NULL
  status         TEXT NOT NULL DEFAULT 'manufactured', -- manufactured|active|disabled
  created_at     INTEGER NOT NULL
);

-- 2. 微信用户
CREATE TABLE users (
  openid        TEXT PRIMARY KEY,
  unionid       TEXT,
  nickname      TEXT,
  created_at    INTEGER NOT NULL,
  last_login_at INTEGER
);

-- 3. 绑定/归属：一台设备至多一个主人（UNIQUE 强制）
CREATE TABLE bindings (
  id       INTEGER PRIMARY KEY AUTOINCREMENT,
  openid   TEXT NOT NULL REFERENCES users(openid),
  mqtt_id  TEXT NOT NULL UNIQUE REFERENCES devices(mqtt_id),
  alias    TEXT,                            -- 用户给唤醒器起的名
  bound_at INTEGER NOT NULL
);
CREATE INDEX idx_bindings_openid ON bindings(openid);

-- 4. 唤醒目标：某绑定下要唤醒的电脑列表
CREATE TABLE wake_targets (
  id         INTEGER PRIMARY KEY AUTOINCREMENT,
  binding_id INTEGER NOT NULL REFERENCES bindings(id) ON DELETE CASCADE,
  target_mac TEXT NOT NULL,                 -- 电脑网卡 MAC(12hex)
  name       TEXT,                          -- “我的台式机”
  created_at INTEGER NOT NULL,
  UNIQUE(binding_id, target_mac)
);

-- 5.（可选）设备运行状态，可由 retain 事件重建，也可放 Redis
CREATE TABLE device_status (
  mqtt_id    TEXT PRIMARY KEY REFERENCES devices(mqtt_id),
  online     INTEGER NOT NULL DEFAULT 0,
  last_seen  INTEGER,
  last_event TEXT
);
```

### 8.4 出厂登记的数据来源(已现成)

固件 `printDeviceId()`(`wol_esp.ino`)输出机读单行:

```
[ID] model=W1 mqtt_id=wol-aabbccddeeff psk=3f9a...
```

产线闭环:**烧录 → 上电 → 采集串口 `[ID]` 行 → 汇成 CSV → 导入 `devices` 表**。
`chip_mac` 由 `mqtt_id` 去掉 `wol-` 前缀即得。

导入做成**二进制子命令**(而非 HTTP 端点,免去暴露需鉴权的管理接口):

```
./server import-devices factory_batch_001.csv
```

### 8.5 表如何串起业务

| 流程 | 用到的表 |
|------|---------|
| **claim 绑定(覆盖式)** | 查 `devices` 校验 psk 与 status(验真) → **覆盖** `bindings` 为当前 openid(不校验旧主) → 轮换 mqtt_pass 存 `devices.mqtt_pass_hash` |
| **EMQX 认证钩子** | `SELECT mqtt_pass_hash FROM devices WHERE mqtt_id=?` 哈希比对(点查,极快) |
| **唤醒** | `wake_targets` 取 target_mac → 经 `binding` 得 mqtt_id → 校验 openid 拥有该 binding → 发 `{cmd:"wol","mac":target_mac}` |

> claim 覆盖旧 binding 时,旧主名下该设备的 `wake_targets` 应一并清理(`ON DELETE
> CASCADE` 随旧 binding 删除即可)。

### 8.6 设计点

- **psk 在登记表的作用是"验真",不是"验主人"**:psk 印在外壳是公开的;归属用
  「配网即重绑」(5.1),不在 DB 层设门槛,防盗绑靠**二维码放置位置**这一物理措施。
- **单主人 vs 共享**:`bindings.mqtt_id UNIQUE` 先锁单主人(claim 覆盖式重绑天然
  契合);将来要家庭共享再加 `device_shares(mqtt_id, openid, role)`,不动现有表。
- **DB 选型**:本负载读多写少(认证=点查,导入=偶发批量),SQLite 开 WAL 足够;
  仅当需多实例后端或高并发写时才上 Postgres——单 Gin 二进制不需要。
- **设备状态 / 微信订阅配额**:`device_status` 可由 retain 事件重建,亦可放 Redis;
  微信一次性订阅配额按需另立轻量表或缓存,不在核心模型内。

## 9. 与现有固件的契合

本架构**不要求改动配网 GATT 协议**:`applyAndSaveConfig()` 已接收
`ssid/wpass/wolmac/mqttsvr/mqttport/mqttuser/mqttpass`,后端只需在 claim 时把
生成的 `mqttuser`(可固定或 = mqtt_id)/`mqttpass`/`mqttsvr` 填进 BLE config 载荷。

固件侧仅需确认:`mqtt_id` 派生值与后端 ownership 表一致(已是芯片 MAC 派生,天然一致)。

## 10. 决策记录:为何不让小程序直连 MQTT

| 维度 | 直连 MQTT | 后端中转(选用) |
|------|----------|----------------|
| 后台/关闭后通知 | ❌ 仍需后端订阅消息 | ✅ 后端统一推 |
| ACL 复杂度 | 每用户动态 ACL + 短期凭据轮换 | 设备模板 + 后端单账号,极简 |
| 控制凭据位置 | 落在客户端 | 仅后端 |
| 域名/备案 | 额外 wss 合法域名 | 复用 API 域名 |
| 客户端复杂度 | MQTT.js + 连接生命周期(onShow/onHide) | 普通 HTTPS |
| 前台实时性 | 最好 | 好(SSE/轮询,WoL 够用) |

关键前提:**后端是必选项**(认证 + 推送 + claim),直连"省后端"的卖点不成立;
小程序不能常驻后台,直连只覆盖前台,价值有限。仅当"前台毫秒级实时是核心卖点"
且愿担 wss 备案 + 每用户凭据轮换时才考虑直连——家用 WoL 不满足。
