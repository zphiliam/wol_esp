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
| **后端中转** | `backend-<n>` | `home/wol/+/cmd` | `home/wol/+/event` |
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

%% 2) 后端中转账号(用 username 匹配,或直接设为 superuser)
{allow, {username, "backend"}, subscribe, ["home/wol/+/event"]}.
{allow, {username, "backend"}, publish,   ["home/wol/+/cmd"]}.
```

> 设备授权用 `${clientid}` 占位符,EMQX 本地即可判定,后端不参与设备授权,
> 省去高频重连的后端压力。

## 4. 认证(EMQX HTTP authenticator)

EMQX 把认证委托给后端。请求体模板:

```json
{ "clientid": "${clientid}", "username": "${username}", "password": "${password}" }
```

后端按主体类型返回:

**设备**(`clientid` 以 `wol-` 开头):校验 `clientid` + `password`(=后端签发的
per-device 随机密码)是否匹配 ownership 表:

```json
{ "result": "allow" }
```

**后端中转账号**:固定强密码,返回 `allow`(授权走第 3 节的 username 规则)。

> 授权全部走第 3 节的 EMQX 本地规则,认证响应不需要再返回 `acl` 字段
> (因为没有"每用户动态 ACL"了——小程序不连 MQTT)。

## 5. 设备身份与凭据签发

⚠️ **设备 MQTT 密码绝不能用公开的 PSK 或可从二维码推导的值**。二维码上的
`mqtt_id` + `PSK` 是公开信息(印在外壳),若密码可由此推出,拍到二维码即可冒充设备。

| 数据 | 来源 | 是否公开 | 用途 |
|------|------|---------|------|
| `mqtt_id` | 芯片 eFuse MAC 派生 | 公开(二维码) | MQTT clientid + topic 标识 |
| `PSK` | 设备首次开机随机生成 | 公开(二维码) | BLE 配网通道加密 + 绑定校验 |
| `mqtt_pass` | **后端随机生成** | **私密** | MQTT 认证;仅经加密 BLE 下发到真设备 |

设备的 MQTT 密码是服务器签发的随机密钥,只通过 PSK 加密的 BLE `config` 通道
进到真设备里——公开二维码本身无法在 MQTT 上冒充设备。

## 6. 时序

### 6.1 配网 + claim 绑定

```
用户        小程序            后端              EMQX        设备
 │  扫码      │                │                 │           │
 │─(mqtt_id, │                │                 │           │
 │   PSK)──▶ │                │                 │           │
 │           │─claim 请求─────▶│                 │           │
 │           │ (mqtt_id,PSK,   │ 校验归属/抢占    │           │
 │           │  微信 openid)    │ 绑设备→用户       │           │
 │           │                │ 生成 mqtt_pass   │           │
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

claim 抢占防护:二维码可能被偷拍。建议 claim 要求设备当前 BLE 可达(在跟前),
或允许"物理 10s 工厂重置后重新 claim"(固件已有 10s 工厂重置)。

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
| POST | `/devices/claim` | 扫码绑定:body `{mqtt_id, psk, ...}`;返回 BLE 下发所需的 mqtt 配置 |
| GET | `/devices` | 当前用户名下设备列表 + 在线状态 |
| POST | `/devices/{id}/wol` | 唤醒:body `{mac?}`;同步等待或返回 task id |
| POST | `/devices/{id}/cmd` | 透传其它指令(reboot/info/led/ota…),后端校验归属 |
| GET | `/devices/{id}/events` | SSE:前台打开时推该设备事件 |
| POST | `/devices/{id}/unbind` | 解绑 |

### 对 EMQX

| 方向 | 机制 |
|------|------|
| 认证 | EMQX → 后端 `POST /mqtt/auth`(第 4 节) |
| 发布指令 | 后端 → EMQX REST `POST /api/v5/publish` |
| 消费事件 | 后端常驻 MQTT client 订 `home/wol/+/event`;或用 EMQX 规则引擎 webhook 把 event POST 给后端 |

> 事件消费二选一:常驻 MQTT 订阅实现简单、实时;规则引擎 webhook 无需常驻连接、
> 更易水平扩展。小规模用常驻订阅即可。

## 8. 存储(后端)

| 数据 | 内容 | 建议存储 |
|------|------|---------|
| ownership | `mqtt_id ↔ openid`、绑定时间、设备别名 | 关系库 |
| 设备凭据 | `mqtt_id → mqtt_pass`(哈希存)、PSK(校验用) | 关系库 |
| 设备状态 | 在线/最后事件/last seen | Redis/缓存(可由 retain 重建) |
| 订阅授权 | 用户的微信一次性订阅配额记录 | 关系库 |

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
