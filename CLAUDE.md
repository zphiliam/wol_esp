# CLAUDE.md — WoL ESP8266/ESP32-C3

## 项目概述

基于 ESP8266（ESP-12F）或 ESP32-C3 SuperMini 的 Wake-on-LAN 控制器，通过 MQTT over TLS 接收远程指令，向局域网内目标电脑发送 WoL 魔法包。同一份代码通过 `#ifdef` 兼容两款硬件，Arduino IDE 选板后自动适配。

## 文件结构

| 文件 | 说明 |
|------|------|
| `wol_esp.ino` | 主程序，所有逻辑均在此文件 |
| `config.h` | 编译期常量（超时参数、行为参数、工厂重置引脚、版本号）；**不含凭据** |
| `PROTOCOL.md` | MQTT 主题与报文格式文档 |

## 硬件

| 项目 | ESP-12F | ESP32-C3 SuperMini |
|------|---------|-------------------|
| 芯片 | ESP8266，板载 CH340 | ESP32-C3，USB-CDC 直连 |
| LED | GPIO2，active LOW | GPIO8，active LOW |
| 重置键 | GPIO0（FLASH 键） | GPIO9（BOOT 键） |
| 串口 | 115200 baud | 115200 baud，需开启 USB CDC on Boot |

## 开发环境

- Arduino IDE，语言为 C++（`.ino` 文件）
- 开发板支持包（Arduino IDE 开发板管理器安装）：
  - ESP-12F：`esp8266` by ESP8266 Community
  - ESP32-C3：`esp32` by Espressif Systems，板型选 `ESP32C3 Dev Module`
- 依赖库（通过 Arduino 库管理器安装）：
  - `PubSubClient` by Nick O'Leary
  - `ArduinoJson` by Benoit Blanchon
- ESP32-C3 分区方案选 **Minimal SPIFFS (1.9MB APP with OTA/190KB SPIFFS)**（支持 OTA；SPIFFS 190KB 足够存 config.json；每个 app 分区 ~1.9MB 可容纳含 TLS 的固件）
  - 旧方案 `No OTA (2MB APP/2MB SPIFFS)` 已弃用，切换后需手动烧录一次

## 配置持久化

凭据存于设备 LittleFS，固件本身不含敏感信息。涉及两个文件：

**`/config.json`** — MQTT 及设备配置：
```json
{
  "wifi_ssid": "...", "wifi_pass": "...",
  "mqtt_server": "...", "mqtt_port": 8883,
  "mqtt_user": "...", "mqtt_pass": "...", "mqtt_id": "...",
  "wol_mac": "AABBCCDDEEFF",
  "wifi_ever_ok": false,
  "ap_full_config": false
}
```
- `wifi_ssid` / `wifi_pass`：记录最近一次成功连接的网络，兼作降级兜底（若 `/wifi_networks.json` 损坏可迁移回来）
- `ap_full_config`：SoftAP 网页是否显示完整 MQTT 配置字段（默认 false，只显示 wifi/mac）

**`/wifi_networks.json`** — WiFi 历史网络列表（最多 5 条，头部 = 最近连接）：
```json
[
  {"ssid": "HomeWiFi",   "pass": "..."},
  {"ssid": "OfficeWiFi", "pass": "..."}
]
```
- 首次启动（旧版升级）时自动从 `config.json` 迁移 `wifi_ssid`/`wifi_pass`
- `config.h` 只保留超时/间隔等行为参数和版本号

## 启动流程

```
setup()
  ├─ LittleFS.begin()
  ├─ loadConfig()            先加载，进任何模式时 cfg 都已填充
  ├─ loadWifiNetworks()      加载历史 WiFi 列表；旧版自动从 config.json 迁移
  ├─ checkReconfigFlag()     检测 /reconfig 标志 → enterConfigMode()（永不返回）
  ├─ checkSoftAPFlag()       检测 /softap 标志   → enterSoftAPMode()（永不返回）
  ├─ if (!configOk || !hasWifi)  无有效配置      → enterConfigMode()（永不返回）
  ├─ 填充 TOPIC_SUB / TOPIC_PUB
  ├─ connectWiFiMulti()      轮试历史列表；全部失败时：首次→configMode，曾连过→restart
  └─ connectMQTT()
```

## 运行时进入配置模式

任意时刻（正常运行、WiFi 连接中、MQTT 重试中）均可触发：

| 操作 | 效果 | LED 反馈 |
|------|------|---------|
| 首次无 `/config.json` | 直接进入串口 CLI | 100ms 快闪 |
| 串口输入 `config` + 回车 | 写 /reconfig → 重启 → 串口 CLI | 无 |
| 串口输入 `ap` + 回车 | 写 /softap → 重启 → SoftAP 网页 | 无 |
| 串口输入 `reboot` + 回车 | 直接重启 | 无 |
| 按住 FLASH/BOOT 键 3s 后松手 | 写 /softap → 重启 → SoftAP 网页 | 3s 时双闪，松手后双闪确认 |
| 按住 FLASH/BOOT 键 10s | 写 /reconfig → 重启 → 串口 CLI（配置保留） | 10s 时五闪确认 |

## SoftAP 模式（enterSoftAPMode）

触发条件：检测到 `/softap` 标志文件。LED 500ms 慢闪。

- AP SSID：`WoL-Setup-XXXX`（XXXX = chip ID 末 4 位十六进制），开放网络
- 浏览器打开 `http://192.168.4.1` 进入配置页面
- 默认显示字段：WiFi SSID、WiFi 密码、WoL MAC
- `ap_full_config=true` 时额外显示：MQTT 服务器、端口、用户名、密码、Client ID
- 保存时将填写的网络**追加**到历史列表头部（不覆盖已有条目），同时写入 `config.json`
- 保存后自动重启进入正常运行模式

**串口 `ap` 命令逻辑**：
- config.json 不存在 → 写入最小配置（ap_full_config=true，其余字段空）→ 写 /softap → 重启
- config.json 存在 → 直接写 /softap → 重启（ap_full_config 沿用现有值）

如需已有配置的情况下强制显示全量字段：`set ap_full_config true` 后再 `ap`。

## 串口配置模式（enterConfigMode）

触发条件：`/reconfig` 标志、无 `/config.json`、或 WiFi 首次连接超时。LED 100ms 快闪。

命令：`set <key> <value>` / `wifi list` / `wifi del <n>` / `show` / `save` / `ap` / `reset` / `reboot` / `help`

- `save`：若 staged 中有 `wifi_ssid`，追加到历史列表头部；写入 LittleFS 并自动重启
- `wifi list`：列出历史 WiFi 网络（索引 + SSID）
- `wifi del <n>`：删除第 n 条历史网络（立即持久化，不重启）
- `set wifi_ssid` / `set wifi_pass`：暂存新网络，配合 `save` 写入历史列表
- `ap`：切换到 SoftAP 网页配置模式
- `reset`：清除 `config.json` 和 `wifi_networks.json` 并重启
- `set ap_full_config true|false`：控制 SoftAP 页面字段范围

## MQTT 主题

```
下行指令：home/wol/<mqtt_id>/cmd
上行事件：home/wol/<mqtt_id>/event
```

- 主题中 `<mqtt_id>` 运行时由 `cfg.mqtt_id` 填充
- `online` / `reconnect` / `offline (LWT)` 均设置 `retain=true`
- 详细报文格式见 `PROTOCOL.md`

## 关键设计说明

- **配置持久化**：凭据存 LittleFS，固件不含敏感信息；串口 CLI 或 SoftAP 网页写入，按键或串口命令触发重配置
- **多网络历史**：`/wifi_networks.json` 存最多 5 条历史 WiFi，头部为最近连接网络；启动时 ESP32-C3 先扫描可见网络排序后轮试，ESP8266 按列表顺序盲试；连接成功后将该网络移至列表头部并持久化
- **ap_full_config**：存于 `/config.json`；false 时 SoftAP 只显示 wifi/mac 字段，true 时显示全部 MQTT 字段；可通过串口 `set ap_full_config true` 切换
- **wifi_ever_ok**：记录是否曾成功连接 WiFi，存于 `/config.json`；所有历史网络全部失败时：从未连过→进配置模式，曾连过→重启；串口 CLI `save` 时强制重置为 `false`，确保重新配置后连不上时能回到配置模式而非无限重启
- **重连计数**：`mqttReconnectCount` 记录本次启动后的 MQTT 重连次数，随 `reconnect` 事件上报
- **首次连接 vs 重连**：`mqttEverConnected` 区分两种场景，首次发 `online`，后续发 `reconnect`
- **安全**：`config` 指令响应不返回 MQTT 密码；密码仅在串口启动日志中输出（本地调试用）
- **TLS**：使用 `WiFiClientSecure` 加密传输，但不验证证书（家用场景）
- **WoL 广播地址**：根据本机 IP 和子网掩码动态计算，无需硬编码

## OTA 升级

```json
{"cmd":"ota","url":"https://github.com/user/repo/releases/download/v2.3.0/firmware.bin"}
```

- 自动跟踪 HTTP 重定向（GitHub release URL 两跳均为 HTTPS，已处理）
- 流程：发布 `ota_start` → 断开 MQTT → 流式下载写入 Flash → 重连 MQTT → 发布 `ota_success`/`ota_fail`
- **ESP8266**：使用 `ESP8266httpUpdate`（内置），自动处理重定向和 TLS
- **ESP32-C3**：使用 `HTTPClient`（`setFollowRedirects`）+ `Update` 库（均内置）
- 固件下载期间 MQTT 断开，进度仅输出串口；成功后设备重启
- ESP32-C3 必须使用支持 OTA 的分区方案（见开发环境一节）

## 上报间隔运行时调整

```json
{"cmd":"set","key":"status_interval","val":60}
```

范围 10～3600 秒，`val=0` 禁用，重启后恢复默认值（`STATUS_INTERVAL_DEFAULT`）。
