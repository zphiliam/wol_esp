# WoL ESP8266/ESP32-C3

通过 MQTT over TLS 远程唤醒局域网内电脑，支持 ESP-12F 和 ESP32-C3 SuperMini 两款硬件，同一份代码编译时自动适配。

凭据（WiFi、MQTT、WoL MAC）存于设备 LittleFS，**固件本身不含敏感信息**，烧录后通过串口 CLI 或 SoftAP 网页写入。支持保存多个 WiFi 网络（最多 5 条），换地方后自动轮试已保存网络并连接。

## 硬件

| 项目 | ESP-12F | ESP32-C3 SuperMini |
|------|---------|-------------------|
| 芯片 | ESP8266 | ESP32-C3 |
| LED | GPIO2，active LOW | GPIO8，active LOW |
| 重置键 | GPIO0（FLASH 键） | GPIO9（BOOT 键） |
| 烧录接口 | 板载 CH340（USB-Serial） | USB-CDC 直连 |

---

## Arduino IDE 环境搭建

### 一、安装 ESP8266 支持

**1. 添加开发板管理器 URL**

打开 Arduino IDE → `File` → `Preferences`（macOS：`Arduino IDE` → `Settings`），在 **Additional boards manager URLs** 填入：

```
http://arduino.esp8266.com/stable/package_esp8266com_index.json
```

如果已有其他 URL，用英文逗号分隔追加。

**2. 安装支持包**

`Tools` → `Board` → `Boards Manager`，搜索 `esp8266`，安装 **esp8266 by ESP8266 Community**。

**3. 选板**

`Tools` → `Board` → `ESP8266 Boards` → **Generic ESP8266 Module**

**4. 烧录参数**

| 参数 | 值 |
|------|----|
| Upload Speed | 115200 |
| Flash Size | 4MB (FS:2MB OTA:~1019KB) |
| Port | 对应 CH340 的串口 |

---

### 二、安装 ESP32-C3 支持

**1. 添加开发板管理器 URL**

同上位置，追加：

```
https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
```

**2. 安装支持包**

`Boards Manager` 搜索 `esp32`，安装 **esp32 by Espressif Systems**（版本 ≥ 2.x）。

**3. 选板**

`Tools` → `Board` → `ESP32 Arduino` → **ESP32C3 Dev Module**

**4. 关键参数**

| 参数 | 值 | 说明 |
|------|----|------|
| USB CDC On Boot | **Enabled** | 必须开启，否则串口无输出 |
| Partition Scheme | **Minimal SPIFFS (1.9MB APP with OTA/190KB SPIFFS)** | 支持 OTA；每个 APP 分区 ~1.9MB 可容纳含 TLS 的固件 |
| Upload Speed | 921600 | |
| Flash Size | 4MB (32Mb) | |

---

### 三、安装依赖库

两款硬件共用相同的库，在 `Tools` → `Manage Libraries` 中安装：

| 库名 | 作者 |
|------|------|
| PubSubClient | Nick O'Leary |
| ArduinoJson | Benoit Blanchon |

---

## 首次烧录与配置

### 1. 烧录固件

直接编译上传，无需修改任何代码。固件不含凭据。

### 2. 写入配置

烧录后设备进入配置模式（LED 快速闪烁），有两种方式写入凭据：

#### 方式 A：SoftAP 网页配置（推荐）

用手机或电脑连接 WiFi `WoL-Setup-XXXX`（开放网络），浏览器打开 `http://192.168.4.1`，填写 WiFi 和 WoL MAC 后保存重启。

> 若需要配置 MQTT 字段，在串口 CLI 中先执行 `set ap_full_config true`，再执行 `ap` 重新进入 SoftAP 页面。

#### 方式 B：串口 CLI

打开串口监视器（115200 baud），逐条发送：

```
set wifi_ssid   <你的WiFi名称>
set wifi_pass   <你的WiFi密码>
set mqtt_server <MQTT服务器地址>
set mqtt_port   8883
set mqtt_user   <MQTT用户名>
set mqtt_pass   <MQTT密码>
set mqtt_id     <设备ID，如 esp_12f_01>
set wol_mac     <目标电脑MAC，12位十六进制无分隔符，如 AABBCCDDEEFF>
save
```

`save` 后设备自动重启进入正常工作模式。填写的 WiFi 会追加到历史列表（不覆盖已有网络）。

**其他 CLI 命令：**
- `show` — 查看当前暂存配置及 WiFi 历史列表
- `wifi list` — 列出所有已保存的 WiFi 网络
- `wifi del <n>` — 删除第 n 条 WiFi 历史记录（从 0 开始）
- `reset` — 清除全部配置（含 WiFi 历史）并重启
- `help` — 显示帮助

### 3. 验证

在 MQTT 客户端订阅 `home/wol/<mqtt_id>/event`，收到如下消息即表示上线成功：

```json
{"event":"online","version":"2.4.0-20260408","uptime":3,"heap":44000,"ip":"192.168.1.x"}
```

---

## 重新配置

正常运行时，可通过以下任意方式重新进入配置模式：

| 操作 | 效果 |
|------|------|
| 串口输入 `ap` + 回车 | 重启进入 SoftAP 网页配置 |
| 串口输入 `config` + 回车 | 重启进入串口 CLI 配置 |
| 串口输入 `reboot` + 回车 | 直接重启 |
| 按住 FLASH/BOOT 键 3 秒后松手 | 重启进入 SoftAP 网页配置 |
| 按住 FLASH/BOOT 键 10 秒 | 重启进入串口 CLI 配置（配置保留） |

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

## OTA 升级

向 `home/wol/<mqtt_id>/cmd` 发送：

```json
{"cmd":"ota","url":"https://github.com/user/repo/releases/download/v2.3.1/firmware.bin"}
```

- 自动跟踪 HTTP 重定向（支持 GitHub release URL）
- 升级期间 MQTT 断开，进度输出到串口
- 成功后设备自动重启，并发布 `ota_success` 事件；失败发布 `ota_fail`
- **ESP32-C3 须使用 Minimal SPIFFS 分区方案**（见上方环境搭建）才能支持 OTA
