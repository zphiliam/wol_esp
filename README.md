# WoL ESP32-C3

基于 ESP32-C3 SuperMini 的 Wake-on-LAN 控制器：通过 MQTT over TLS 接收远程指令，向局域网内目标电脑发送 WoL 魔法包。v3.0 起为面向终端用户的版本，配网统一走低功耗蓝牙（BLE），不再兼容 ESP8266（无蓝牙射频）。

凭据（WiFi、MQTT、WoL MAC）存于设备 LittleFS，**固件本身不含敏感信息**。支持保存多个 WiFi 网络（最多 5 条），换地方后自动轮试已保存网络并连接。

## 硬件（ESP32-C3 SuperMini）

| 项目 | 说明 |
|------|------|
| 芯片 | ESP32-C3，USB-CDC 直连 |
| LED | GPIO8，active LOW |
| 配置键 | GPIO9（BOOT 键），active LOW |
| 串口 | 115200 baud，需开启 USB CDC on Boot |

---

## Arduino IDE 环境搭建

**1. 添加开发板管理器 URL**

`Arduino IDE` → `Settings` → **Additional boards manager URLs** 追加：

```
https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
```

**2. 安装支持包**

`Tools` → `Board` → `Boards Manager`，搜索 `esp32`，安装 **esp32 by Espressif Systems**。

**3. 选板**：`Tools` → `Board` → `ESP32 Arduino` → **ESP32C3 Dev Module**

**4. 关键参数**

| 参数 | 值 | 说明 |
|------|----|------|
| USB CDC On Boot | **Enabled** | 必须开启，否则串口无输出 |
| Partition Scheme | **Minimal SPIFFS (1.9MB APP with OTA/190KB SPIFFS)** | 支持 OTA；~1.9MB app 分区可容纳含 TLS + BLE 的固件 |
| Upload Speed | 921600 | |
| Flash Size | 4MB (32Mb) | |

**5. 依赖库**（`Tools` → `Manage Libraries`）

| 库名 | 作者 |
|------|------|
| PubSubClient | Nick O'Leary |
| ArduinoJson | Benoit Blanchon |
| NimBLE-Arduino | h2zero |

---

## 首次烧录与配置

### 1. 烧录固件

直接编译上传，无需修改任何代码。固件不含凭据。

### 2. BLE 配网（默认入口）

首次开机无配置时设备自动进入 BLE 配网模式（LED 500ms 慢闪），BLE 设备名为 `WoL-XXXX`（XXXX = chip ID 末 4 位十六进制）。

配网方式：用配网 App（微信小程序，开发中；`test/ble_test.html` 为开发参考实现）连接设备 → X25519 握手 → 扫描并选择 WiFi → 填表单 → 加密下发 → 设备保存重启。

配网载荷经 X25519 ECDH + AES-256-GCM 加密，密钥由设备 per-device PSK 派生。PSK 通过设备二维码获取，详见 `docs/BLE_REDESIGN.md`。

### 3. 串口 CLI（隐藏恢复通道）

不对终端用户宣传，用于产线初配与变砖恢复。串口（115200 baud）输入 `config` + 回车 → 重启进入 CLI：

```
set wifi_ssid   <你的WiFi名称>
set wifi_pass   <你的WiFi密码>
set mqtt_server <MQTT服务器地址>
set mqtt_port   8883
set mqtt_user   <MQTT用户名>
set mqtt_pass   <MQTT密码>
set wol_mac     <目标电脑MAC，12位十六进制无分隔符，可选>
save
```

`mqtt_id` 不可配置——由芯片 eFuse MAC 自动派生（`wol-<12位 MAC 十六进制>`）。`save` 后设备自动重启进入正常工作模式。

**其他 CLI 命令：** `show` / `wifi list` / `wifi del <n>` / `id` / `ble` / `reset` / `reboot` / `help`。其中 `id` 打印设备 `mqtt_id` 与 BLE PSK（hex），用于贴二维码 / 服务端配置。

### 4. 验证

在 MQTT 客户端订阅 `home/wol/<mqtt_id>/event`，收到如下消息即表示上线成功：

```json
{"event":"online","version":"3.0.0-20260518","uptime":3,"heap":44000,"ip":"192.168.1.x"}
```

---

## 重新配置

| 操作 | 效果 |
|------|------|
| 按住 BOOT 键 3s 后松手 | 重启进入 BLE 配网（配置保留），3s 时双闪确认 |
| 按住 BOOT 键 10s | 工厂重置（清空配置）→ 重启 → BLE 配网，10s 时五闪确认 |
| 串口输入 `ble` + 回车 | 重启进入 BLE 配网 |
| 串口输入 `config` + 回车 | 重启进入串口 CLI（隐藏恢复通道，配置保留） |
| 串口输入 `reboot` + 回车 | 直接重启 |

---

## MQTT 主题

```
下行指令：home/wol/<mqtt_id>/cmd
上行事件：home/wol/<mqtt_id>/event
```

详细报文格式见 [PROTOCOL.md](PROTOCOL.md)。

## 常用指令

```json
{"cmd":"wol"}                                        // 唤醒配置中的目标电脑
{"cmd":"wol","mac":"AABBCCDDEEFF"}                   // 唤醒指定 MAC 的电脑
{"cmd":"ping"}                                       // 测试连通性
{"cmd":"info"}                                       // 查询设备信息
{"cmd":"set","key":"status_interval","val":60}       // 设置心跳间隔（秒），0 为禁用
{"cmd":"reboot"}                                     // 重启设备
{"cmd":"ota","url":"https://.../firmware.bin"}       // OTA 空中升级
```

> 设备内 `wol_mac` 留空时，未带 `mac` 的 `wol` 指令回 `error:no_mac` 事件，不发包。

## OTA 升级

向 `home/wol/<mqtt_id>/cmd` 发送：

```json
{"cmd":"ota","url":"https://github.com/user/repo/releases/download/v3.0.0/firmware.bin"}
```

- 自动跟踪 HTTP 重定向（支持 GitHub release URL）
- 升级期间 MQTT 断开，进度输出到串口
- 成功后设备自动重启，并发布 `ota_success` 事件；失败发布 `ota_fail`
- 须使用 **Minimal SPIFFS 分区方案**（见上方环境搭建）才能支持 OTA

---

## 文档

| 文档 | 说明 |
|------|------|
| [PROTOCOL.md](PROTOCOL.md) | MQTT 主题与报文格式 |
| [docs/BLE_REDESIGN.md](docs/BLE_REDESIGN.md) | BLE 配网设计与 GATT 协议、二维码契约 |
| [docs/ARCHITECTURE_v3.md](docs/ARCHITECTURE_v3.md) | 服务端架构：自建 EMQX + 后端中转 + 微信小程序 |
| [CLAUDE.md](CLAUDE.md) | 项目总览（面向开发） |
