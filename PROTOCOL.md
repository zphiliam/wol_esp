# WoL ESP32-C3 — MQTT 协议文档

## 主题

主题中的 `{mqtt_id}` 由 `/config.json` 中的 `mqtt_id` 字段决定，默认示例值为 `esp_12f_01`。

| 方向 | 主题 |
|------|------|
| 下行（指令） | `home/wol/{mqtt_id}/cmd` |
| 上行（事件） | `home/wol/{mqtt_id}/event` |

---

## 下行指令

所有指令均为 JSON 格式，发送到 `cmd` 主题。

### 唤醒电脑

**推荐**：每次随包指定目标 MAC（用户在手机端管理多台电脑）：
```json
{"cmd":"wol","mac":"AABBCCDDEEFF"}
```

省略 `mac` 字段时，使用设备配置中的 `wol_mac` 作为兜底：
```json
{"cmd":"wol"}
```

> - `mac` 格式：12 位十六进制字符，无分隔符，大小写均可
> - 若指令未带 `mac` 且设备未存默认 MAC（`wol_mac` 为空），返回 `error:no_mac` 事件，不发包

**唤醒验证**：发包后探测目标是否真的上线（用于"测试唤醒"）：
```json
{"cmd":"wol","mac":"AABBCCDDEEFF","verify":true,"ip":"192.168.1.5"}
```

> - `verify` 可选，缺省 `false` 时行为与上文一致（仅发包 + `wol` 事件，向后兼容）
> - `ip` 可选提示（上次已知 IP，加速探测）；设备最终按 **MAC** 判定上线（DHCP 换 IP 也能命中）
> - 流程：立即发魔法包 + `wol` 事件 → 每秒探测局域网 ARP 表找该 MAC → 命中回报 `wake_result(ok:true)`；
>   超时（`WAKE_VERIFY_TIMEOUT_MS`，默认 60s）回报 `wake_result(ok:false, reason:"timeout")`

### 测试连通性
```json
{"cmd":"ping"}
```

### 重启设备
```json
{"cmd":"reboot"}
```

### 查询设备运行状态
```json
{"cmd":"info"}
```

响应包含：version、当前连接 ssid/rssi、wol_mac（未配置时为空字符串）、MQTT 重连次数，以及公共字段 uptime/heap/ip。

### 查询完整配置
```json
{"cmd":"config"}
```

响应包含：所有已保存配置字段（含 WiFi 历史列表）及公共字段。

### 控制 LED

```json
{"cmd":"led","val":"on"}
{"cmd":"led","val":"off"}
{"cmd":"led","val":"toggle"}
{"cmd":"led","val":"query"}
```

闪烁（`times` 默认 5，`interval` 默认 500ms）：
```json
{"cmd":"led","val":"blink","times":5,"interval":500}
```

> `times` 范围：1 ~ 10
> `interval` 范围：100 ~ 2000 ms

### 设置状态上报间隔

```json
{"cmd":"set","key":"status_interval","val":300}
```

> `val` 单位：秒
> `val` 范围：10 ~ 3600
> `val` 为 0 时禁用状态上报

### 在线更新 MQTT 连接配置

```json
{"cmd":"set_mqtt","server":"new.broker.example.com","port":8883,"user":"newuser","pass":"newpass"}
```

- 可改字段：`server` / `port` / `user` / `pass`，均为可选，省略的字段保留当前值；
  但**至少需提供一个与当前配置不同的字段**，否则返回 `error:set_mqtt_no_change`
- `mqtt_id` 由芯片 eFuse MAC 派生、不可更改，故 topic 不随之变化（无 `id` 字段）
- 执行流程：
  1. 先清除当前 broker 上的 retained 事件（发空 payload），再断开当前 MQTT 连接
  2. 用独立临时客户端测试新配置的连接与认证（单次 TCP 超时 10s）
  3. **测试成功** → 写入 `/config.json` → **设备重启** → 以新配置连接新 broker，
     新 broker 上的 `online` 事件即为切换成功的隐式确认（不单独发成功事件）
  4. **测试失败** → 保留原配置 → 重连原 broker 发布 `error:mqtt_connect_failed`（附 `rc`）→ **重启**

### OTA 固件升级

```json
{"cmd":"ota","url":"https://github.com/user/repo/releases/download/v2.3.0/firmware.bin"}
```

- 支持 HTTP / HTTPS，自动跟踪 HTTP 重定向（GitHub release URL 需跳转，已处理）
- TLS 不验证证书（与 MQTT 连接策略一致）
- 升级期间设备暂停处理其他指令（通常 30s 内完成）
- 成功后自动重启，恢复正常运行；失败后继续运行并上报错误

> **注意**：ESP32-C3 需使用支持 OTA 的分区方案（如 `Minimal SPIFFS`），初次切换须手动烧录一次。

### 网络测速

```json
{"cmd":"speedtest"}
```

自定义测速 URL 和时长：
```json
{"cmd":"speedtest","url":"http://mirrors.tuna.tsinghua.edu.cn/speedtest/100mb.bin","max_seconds":10}
```

- `url`：可选，默认为清华镜像站 100MB 测速文件（**仅支持 HTTP，不支持 HTTPS**）
- `max_seconds`：可选，测速时长上限，范围 5～60，默认 15 秒；达到时限或文件下载完毕即停止
- 下载内容直接丢弃，不写入 Flash
- MQTT 连接全程保持，测速结束后立即上报结果
- 计时从收到首字节开始，不含 TCP 握手和 HTTP 头部解析耗时
- 执行前检查 heap 连续可用块，不足 6144 字节时拒绝并返回 `error:heap_low`

### 局域网发现（net_scan）

```json
{"cmd":"net_scan"}
```

- 扫描设备所在 /24 网段，发现在线主机（IP + MAC），并尝试解析主机名（NBNS / mDNS 反查）
- 结果经 `net_scan_start` + 多个 `net_scan_result` 事件**分批异步上报**（见下）
- 扫描全程非阻塞，约 10～15s 完成；进行中再收到 `net_scan` 返回 `error:net_scan_busy`
- 仅扫自身 /24，结果上限 64 台；已排除设备自身与网关
- 串口命令 `netscan`（正常运行模式）可单机触发，结果打印到串口，便于不依赖后端验证

---

## 上行事件

所有事件均发布到 `event` 主题。

### 公共字段说明

| 字段 | 类型 | 说明 |
|------|------|------|
| `event` | string | 事件名称 |
| `uptime` | number | 设备运行时间（秒） |
| `heap` | number | 剩余堆内存（字节） |
| `ip` | string | 设备 IP（部分事件携带） |

---

### 设备上线
触发时机：本次启动后首次连接 MQTT 成功
消息属性：`retain=true`，新订阅的客户端会立即收到此消息
```json
{
  "event": "online",
  "version": "2.0.0-20260328",
  "uptime": 0,
  "heap": 45000,
  "ip": "192.168.1.100"
}
```

### MQTT 重连
触发时机：运行中 MQTT 断线后重新连接成功（首次上线不触发）
消息属性：`retain=true`，覆盖 broker 上缓存的 offline 状态
```json
{
  "event": "reconnect",
  "count": 1,
  "uptime": 3610,
  "heap": 44000,
  "ip": "192.168.1.100"
}
```

> `count`：本次启动后的累计重连次数，从 1 开始

### 设备离线（LWT）
触发时机：设备异常断线，由 broker 自动发出
消息属性：`retain=true`，新订阅的客户端会立即收到此消息
```json
{"event":"offline"}
```

### 定时状态上报
触发时机：每隔 `status_interval` 秒自动发送
```json
{
  "event": "status",
  "uptime": 3600,
  "heap": 44000
}
```

### WoL 已触发
```json
{
  "event": "wol",
  "mac": "AABBCCDDEEFF",
  "uptime": 123,
  "heap": 44000,
  "ip": "192.168.1.100"
}
```

> `mac`：实际发送魔法包的目标 MAC 地址（来自命令参数或设备配置）

### 唤醒验证结果（wake_result）

`wol` 指令带 `verify:true` 时，探测结束后发出：
```json
{"event":"wake_result","ok":true,"mac":"AABBCCDDEEFF","ip":"192.168.1.5","elapsed_ms":23400,"uptime":120,"heap":44000,"ip":"..."}
{"event":"wake_result","ok":false,"mac":"AABBCCDDEEFF","reason":"timeout","uptime":180,"heap":44000}
```

> - `ok:true` 时带探测到的 `ip` 与 `elapsed_ms`（从发包到确认上线的耗时）
> - `ok:false` 时 `reason` 取值目前为 `timeout`（60s 内未探测到该 MAC 上线）

### Ping 响应
```json
{
  "event": "pong",
  "uptime": 123,
  "heap": 44000
}
```

### 重启通知
```json
{
  "event": "reboot",
  "reason": "cmd",
  "uptime": 123,
  "heap": 44000
}
```

> `reason` 取值：
> - `cmd`：收到 reboot 指令
> - `24h`：24 小时定时重启

### info 响应
```json
{
  "event": "info",
  "version": "2.4.0-20260408",
  "ssid": "HomeWifi",
  "rssi": -65,
  "wol_mac": "00E269666C6E",
  "reconnects": 0,
  "uptime": 123,
  "heap": 44000,
  "ip": "192.168.1.100"
}
```

> `reconnects`：本次启动后的 MQTT 累计重连次数

### config 响应
```json
{
  "event": "config",
  "wifi_ssid": "HomeWifi",
  "wifi_networks": ["HomeWifi", "OfficeWifi"],
  "mqtt_server": "broker.example.com",
  "mqtt_port": 8883,
  "mqtt_user": "esp_user",
  "mqtt_id": "esp_12f_01",
  "wol_mac": "00E269666C6E",
  "status_interval": 300,
  "wifi_tx_power": 15,
  "uptime": 123,
  "heap": 44000,
  "ip": "192.168.1.100"
}
```

> - `mqtt_pass` 出于安全考虑不予返回
> - `wifi_networks`：历史 WiFi 列表（仅 SSID，不含密码），按最近连接顺序排列
> - `wol_mac`：未配置时为空字符串

### 测速事件

测速开始（HTTP 连接建立前发出）：
```json
{
  "event": "speedtest_start",
  "max_seconds": 15,
  "uptime": 3600,
  "heap": 13000
}
```

测速成功：
```json
{
  "event": "speedtest_result",
  "bytes": 2621440,
  "elapsed_ms": 10023,
  "kbps": 2092,
  "uptime": 3615,
  "heap": 11000
}
```

> `kbps`：下载速率（千比特/秒），`bytes × 8 / elapsed_ms`

测速失败：
```json
{
  "event": "speedtest_fail",
  "reason": "http.begin failed",
  "uptime": 3615,
  "heap": 12000
}
```

### 局域网发现事件（net_scan）

扫描开始（发现前发出一次）：
```json
{
  "event": "net_scan_start",
  "subnet": "192.168.1.0/24",
  "hosts": 254,
  "uptime": 3600,
  "heap": 44000,
  "ip": "192.168.1.100"
}
```

扫描结果（分批，每批 ≤5 台）：
```json
{
  "event": "net_scan_result",
  "seq": 0,
  "last": false,
  "hosts": [
    {"ip":"192.168.1.5","mac":"AABBCCDDEEFF","name":"DESKTOP-A7K2","src":"nbns"},
    {"ip":"192.168.1.13","mac":"DE12AB34CD56","rand":true}
  ],
  "uptime": 3608,
  "heap": 43000,
  "ip": "192.168.1.100"
}
```

> - `seq`：批次序号，从 0 递增
> - `last`：是否为末批；零结果时也会发一批空 `hosts` + `last:true`
> - `name` / `src`：解析到主机名时携带，`src` 取值 `nbns`（Windows）/ `mdns`（macOS/Linux）；无名时两字段省略
> - `rand`：MAC 为本地管理地址（首字节 bit1 置位）时为 `true`，大概率是开启随机 MAC 的手机/设备

### OTA 事件

升级开始（MQTT 断开前发出）：
```json
{
  "event": "ota_start",
  "url": "https://...",
  "uptime": 3600,
  "heap": 44000
}
```

升级成功（重启前发出）：
```json
{
  "event": "ota_success",
  "uptime": 3650,
  "heap": 32000
}
```

升级失败（恢复运行后发出）：
```json
{
  "event": "ota_fail",
  "reason": "HTTP 404",
  "uptime": 3650,
  "heap": 40000
}
```

> OTA 过程中 MQTT 断开，进度仅输出到串口。成功后设备重启，broker 上的 `offline` LWT 会被新的 `online` 覆盖。

### LED 状态响应

```json
{"event":"led:on",     "uptime":123,"heap":44000}
{"event":"led:off",    "uptime":123,"heap":44000}
{"event":"led:toggle", "uptime":123,"heap":44000}
{"event":"led:blink",  "uptime":123,"heap":44000}
```

### set 操作响应

```json
{"event":"ok:status_interval_updated", "uptime":123,"heap":44000}
{"event":"ok:status_disabled",         "uptime":123,"heap":44000}
```

### set_mqtt 操作响应

成功时设备**直接重启**，不在原 broker 发送成功事件；切换成功以**新 broker 上收到的 `online` 事件**为准（见「设备上线」）。

失败（保留原配置，在原 broker 发布后重启）：
```json
{"event":"error:mqtt_connect_failed","rc":-4,"uptime":123,"heap":44000}
```

> `rc`：PubSubClient 连接状态码（负数 = TCP/TLS 错误；`1`=协议版本错误；`4`=认证失败；`5`=未授权；详见 PubSubClient 文档）

---

## 错误响应

| event | 原因 |
|-------|------|
| `error:json_parse` | JSON 格式错误 |
| `error:unknown_cmd` | 未知 cmd 字段 |
| `error:unknown_key` | set 指令中未知 key |
| `error:invalid_value` | val 值非法（如负数） |
| `error:status_interval_too_short` | status_interval 小于 10s |
| `error:status_interval_too_long` | status_interval 大于 3600s |
| `error:unknown_led_val` | 未知 LED val 值 |
| `error:invalid_mac` | wol 指令中 `mac` 参数格式非法 |
| `error:no_mac` | wol 指令未带 `mac` 且设备未存默认 `wol_mac` |
| `error:ota_url_missing` | ota 指令缺少 `url` 字段 |
| `error:heap_low` | speedtest 执行时 heap 连续可用块不足 6144 字节 |
| `error:set_mqtt_no_change` | set_mqtt 指令所有字段均与当前配置相同 |
| `error:mqtt_connect_failed` | set_mqtt 指令测试连接失败，原配置保留（附 `rc` 字段）|
| `error:net_scan_busy` | net_scan 指令在上一次扫描仍进行时收到 |
