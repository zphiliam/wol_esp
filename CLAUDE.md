# CLAUDE.md — WoL ESP32-C3

## 项目概述

基于 ESP32-C3 SuperMini 的 Wake-on-LAN 控制器，通过 MQTT over TLS 接收远程指令，
向局域网内目标电脑发送 WoL 魔法包。v3.0 起为面向终端用户的版本：配网统一走低功耗
蓝牙（BLE），不再兼容 ESP8266（无蓝牙射频）。

## 关联仓库

| 仓库 | 路径 | 说明 |
|------|------|------|
| `esp_auth` | `../esp_auth` | v3 后端服务（Go + SQLite）：微信小程序 API、EMQX HTTP 认证钩子（`POST /mqtt/auth`）、配网即重绑（claim）、指令中转与设备状态。落地 `docs/ARCHITECTURE_v3.md` 设计 |
| `wolapp` | `../wolapp` | 微信小程序（uniapp Vue3，mp-weixin）：扫码 BLE 加密配网、管理控制器与目标电脑（1:N）、一键唤醒（下发携带电脑 MAC）。BLE 加密栈对接 `docs/BLE_REDESIGN.md`（X25519/AES-GCM/HMAC-SHA256） |

## 文件结构

| 文件 | 说明 |
|------|------|
| `wol_esp.ino` | 主程序，所有逻辑均在此文件 |
| `config.h` | 编译期常量（超时参数、行为参数、配置触发引脚、版本号）；**不含凭据** |
| `PROTOCOL.md` | MQTT 主题与报文格式文档 |
| `docs/BLE_REDESIGN.md` | BLE 配网设计方案与 GATT 协议 |
| `docs/ARCHITECTURE_v3.md` | 服务端架构：自建 EMQX + 后端中转 + 微信小程序（含权限/claim/时序） |
| `test/ble_test.html` | Web Bluetooth 配网测试页（开发用） |

## 硬件（ESP32-C3 SuperMini）

| 项目 | 说明 |
|------|------|
| 芯片 | ESP32-C3，USB-CDC 直连 |
| LED | GPIO8，active LOW |
| 配置键 | GPIO9（BOOT 键），active LOW |
| 串口 | 115200 baud，需开启 USB CDC on Boot |

## 开发环境

- Arduino IDE，语言为 C++（`.ino` 文件）
- 开发板支持包：`esp32` by Espressif Systems，板型选 `ESP32C3 Dev Module`
- 依赖库（Arduino 库管理器安装）：
  - `PubSubClient` by Nick O'Leary
  - `ArduinoJson` by Benoit Blanchon
  - `NimBLE-Arduino` by h2zero
- 分区方案选 **Minimal SPIFFS (1.9MB APP with OTA/190KB SPIFFS)**（支持 OTA；
  SPIFFS 190KB 足够存 config.json；~1.9MB app 分区可容纳含 TLS + BLE 的固件）

## 配置持久化

凭据存于设备 LittleFS，固件本身不含敏感信息。涉及两个文件：

**`/config.json`** — MQTT 及设备配置：
```json
{
  "wifi_ssid": "...", "wifi_pass": "...",
  "mqtt_server": "...", "mqtt_port": 8883,
  "mqtt_user": "...", "mqtt_pass": "...",
  "wol_mac": "AABBCCDDEEFF",
  "wifi_ever_ok": false,
  "wifi_tx_power": 15
}
```

- `mqtt_id` 不在此文件中:由芯片 eFuse MAC 派生(`wol-<12位 MAC 十六进制>`),
  开机时 `genMqttId()` 填充 `cfg.mqtt_id`,全局唯一、重启稳定,不可配置。
- BLE PSK 存于独立文件 `/ble_psk.bin`(见 BLE 配网章节)。
- `wifi_ssid` / `wifi_pass`：记录最近一次成功连接的网络，兼作降级兜底（若
  `/wifi_networks.json` 损坏可迁移回来）
- `wol_mac` **可选**：终端形态下用户在手机端管理多台目标 PC，下发 `wol` 指令时
  随包携带 `mac` 字段。设备内 `wol_mac` 留空时，未带 `mac` 的 `wol` 指令回
  `error:no_mac` 事件，不发包。配置入口非空时仍校验为 12 位十六进制。

**`/wifi_networks.json`** — WiFi 历史网络列表（最多 5 条，头部 = 最近连接）：
```json
[
  {"ssid": "HomeWiFi",   "pass": "..."},
  {"ssid": "OfficeWiFi", "pass": "..."}
]
```

## 启动流程

```
setup()
  ├─ LittleFS.begin()
  ├─ loadConfig()            先加载，进任何模式时 cfg 都已填充
  ├─ loadWifiNetworks()      加载历史 WiFi 列表
  ├─ checkReconfigFlag()     检测 /reconfig 标志 → enterConfigMode()（串口 CLI，永不返回）
  ├─ checkBLEProvFlag()      检测 /bleprov 标志  → enterBLEProvMode()（永不返回）
  ├─ if (!configOk || !hasWifi)  无有效配置      → enterBLEProvMode()（永不返回）
  ├─ 填充 TOPIC_SUB / TOPIC_PUB
  ├─ connectWiFiMulti()      轮试历史列表；全部失败时：首次→BLE 配网，曾连过→restart
  └─ connectMQTT()
```

## 配网入口

| 操作 | 效果 | LED 反馈 |
|------|------|---------|
| 首次开机无 `/config.json` | 自动进入 BLE 配网 | 500ms 慢闪 |
| WiFi 历史网络全部连不上（首次） | 自动进入 BLE 配网 | 500ms 慢闪 |
| 按住 BOOT 键 3s 后松手 | 写 /bleprov → 重启 → BLE 配网（配置保留） | 3s 时双闪确认 |
| 按住 BOOT 键 10s | 工厂重置（清空配置）→ 重启 → BLE 配网 | 10s 时五闪确认 |
| 串口输入 `ble` + 回车 | 写 /bleprov → 重启 → BLE 配网 | 无 |
| 串口输入 `config` + 回车（隐藏恢复通道） | 写 /reconfig → 重启 → 串口 CLI | 无 |
| 串口输入 `reboot` + 回车 | 直接重启 | 无 |

## BLE 配网模式（enterBLEProvMode）

阻塞式特殊模式，永不返回，完成后重启。LED 未连接 500ms 慢闪 / 已连接 150ms 快闪。

- BLE 设备名：`WoL-XXXX`（XXXX = chip ID 末 4 位十六进制）
- NimBLE GATT 服务（UUID `e0c1a700-…`），含 5 个特征：
  - `command`（写）：文本指令 `scan` / `reboot`
  - `status`（读+通知）：状态文本（`idle` / `scanning` / `ok:rebooting` / `error:…`）
  - `scan`（读+通知）：WiFi 扫描结果 JSON，分片下发
  - `config`（写）：加密配置载荷，分片上传
  - `handshake`（写+通知）：X25519 公钥交换
- 分片协议：每片头 4 字节 `[总长 LE:2][序号:1][标志:1]`，标志 bit0=末片
- 加密：客户端写 32B X25519 公钥握手 → `session_key = HMAC-SHA256(PSK,
  shared‖"wol-ble-v1")`；`config` 载荷 = `iv(12)‖tag(16)‖密文`，AES-256-GCM
- 配置 JSON 字段：`ssid` `wpass` `wolmac` `mqttsvr` `mqttport` `mqttuser`
  `mqttpass`；含 `mqttsvr` 时整套 MQTT 字段一并写入（`mqtt_id` 自动派生，不传）
- PSK：首次开机随机生成 16B 存 `/ble_psk.bin`（工厂重置不删），串口打印 hex
- 校验落盘走公共函数 `applyAndSaveConfig()`，成功后重启
- 超时回正常运行：仅对已有配置的设备生效（无配置设备常驻配网）
- 完整协议见 `docs/BLE_REDESIGN.md`

阶段进度：阶段 0/1/2 + 固件收尾已完成；阶段 3（微信小程序）待启动。

## 串口配置模式（enterConfigMode）

**隐藏恢复通道**，不对终端用户宣传，用于产线初配与变砖恢复。触发条件：`/reconfig`
标志（由串口 `config` 命令写入）。LED 100ms 快闪。

命令：`set <key> <value>` / `wifi list` / `wifi del <n>` / `id` / `show` / `save` /
`ble` / `reset` / `reboot` / `help`

- `save`：若 staged 中有 `wifi_ssid`，追加到历史列表头部；写入 LittleFS 并自动重启
- `wifi list` / `wifi del <n>`：列出 / 删除历史 WiFi 网络
- `id`：打印设备 `mqtt_id`（芯片派生）与 BLE PSK（hex），用于贴二维码 / 服务端配置。
  同一命令在**正常运行**与 **BLE 配网模式**的串口也可用（无需重启 / 切模式）；
  除人读框外另输出机读单行 `[ID] mqtt_id=... psk=...`，便于采集工具解析
- `ble`：切换到 BLE 配网模式
- `reset`：清除 `config.json` 和 `wifi_networks.json` 并重启

## MQTT 主题

```
下行指令：home/wol/<mqtt_id>/cmd
上行事件：home/wol/<mqtt_id>/event
```

- 主题中 `<mqtt_id>` 运行时由 `cfg.mqtt_id` 填充
- `online` / `reconnect` / `offline (LWT)` 均设置 `retain=true`
- 详细报文格式见 `PROTOCOL.md`

## 关键设计说明

- **配置持久化**：凭据存 LittleFS，固件不含敏感信息；BLE 配网或串口 CLI 写入，
  按键或串口命令触发重配置
- **多网络历史**：`/wifi_networks.json` 存最多 5 条历史 WiFi，头部为最近连接网络；
  启动时先扫描可见网络排序后轮试；连接成功后将该网络移至列表头部并持久化
- **wifi_ever_ok**：记录是否曾成功连接 WiFi，存于 `/config.json`；所有历史网络全部
  失败时：从未连过→进 BLE 配网，曾连过→重启；串口 CLI `save` 时强制重置为 `false`，
  确保重新配置后连不上时能回到配网模式而非无限重启
- **重连计数**：`mqttReconnectCount` 记录本次启动后的 MQTT 重连次数，随 `reconnect`
  事件上报
- **首次连接 vs 重连**：`mqttEverConnected` 区分两种场景，首次发 `online`，后续发
  `reconnect`
- **安全**：`config` 指令响应不返回 MQTT 密码；密码仅在串口启动日志中输出（本地调试用）
- **TLS**：使用 `WiFiClientSecure` 加密传输，但不验证证书（家用场景）
- **WoL 广播地址**：根据本机 IP 和子网掩码动态计算，无需硬编码

## OTA 升级

```json
{"cmd":"ota","url":"https://github.com/user/repo/releases/download/v3.0.0/firmware.bin"}
```

- 自动跟踪 HTTP 重定向（GitHub release URL 两跳均为 HTTPS，已处理）
- 流程：发布 `ota_start` → 断开 MQTT → 流式下载写入 Flash → 重连 MQTT → 发布
  `ota_success`/`ota_fail`
- 使用 `HTTPClient` + `Update` 库（均内置），手动逐跳跟踪重定向
- 固件下载期间 MQTT 断开，进度仅输出串口；成功后设备重启

## 上报间隔运行时调整

```json
{"cmd":"set","key":"status_interval","val":60}
```

范围 10～3600 秒，`val=0` 禁用，重启后恢复默认值（`STATUS_INTERVAL_DEFAULT`）。
