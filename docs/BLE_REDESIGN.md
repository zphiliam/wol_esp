# BLE 配网重设计方案

> 状态:设计已定稿,实现进行中
> 分支:`feature/ble-provisioning`
> 目标版本:面向终端用户的 ESP32-C3 独占版本

## 1. 背景与目标

现有固件用 SoftAP 网页配网,需要用户离开 App → 进手机 WiFi 设置 → 连一个陌生
SSID → 再回来,体验差。新版本改为 **BLE 配网**:按键进入配网模式 → 设备 BLE
广播 → 微信小程序连上 → 下发配置。这是消费级 IoT 的主流范式,全程在一个 App 内
完成。

配套诉求:小程序后台维护最新的 MQTT 服务器与认证凭据,设备换服务器时由小程序
重新下发,无需改固件。

## 2. 已锁定的决策

| 项 | 决策 |
|----|------|
| 平台 | **ESP32-C3 独占**,移除全部 ESP8266 兼容代码(ESP8266 无蓝牙) |
| 配网方式 | 纯 BLE,移除 SoftAP 网页配网及 HTML/HTTP/DNS 相关代码 |
| 配网入口 | 按键触发 + 首次开机无配置自动进入 |
| 广播策略 | 仅配网模式广播;正常运行**不**常驻广播(配网时用户在设备跟前) |
| 串口 CLI | **保留**,作为隐藏的产线初配 / 变砖恢复通道(不对终端用户宣传) |
| 客户端 | 自建 GATT 服务,微信小程序 + 通用 BLE 调试 App(nRF Connect) |
| 配置字段 | 全套:WiFi + WoL MAC + MQTT 服务器/端口/用户/密码/ClientID |
| 安全 | per-device PSK + ECDH(Curve25519)+ AES-GCM 应用层加密 |
| BLE 协议栈 | NimBLE(轻量,必须;不用 Bluedroid) |

## 3. 启动流程

```
setup()
  ├─ LittleFS.begin()
  ├─ loadConfig() / loadWifiNetworks()
  ├─ checkButtonAtBoot()        长按检测 → 工厂重置
  ├─ if (!configOk || !hasWifi) 无有效配置 → enterBLEProvMode()
  ├─ connectWiFiMulti()
  └─ connectMQTT()
```

`/reconfig`、`/softap` 标志文件机制废弃。串口输入 `config` 仍可即时进 CLI(恢复
通道)。

## 4. 按键方案

| 触发 | 行为 | LED |
|------|------|-----|
| 首次开机无配置 | 自动进 BLE 配网 | 慢闪 |
| 按住 3s 松手 | BLE 配网(保留现有配置,只覆盖填写项) | 3s 时双闪 |
| 按住 10s | 清空 config.json + wifi_networks.json → BLE 配网 | 10s 时五闪 |
| 配网 5 分钟无连接 | 自动重启回正常运行 | — |
| 串口输入 `config`(隐藏) | 串口 CLI,产线/恢复用 | 快闪 |

## 5. BLE 配网模式 `enterBLEProvMode()`

阻塞式特殊模式(类比原 `enterSoftAPMode()`),永不返回,完成后重启。

### 广播

- 设备名 `WoL-XXXX`(chip ID 末 4 位十六进制)
- 广播包内带 deviceId,供小程序匹配后台凭据
- 进入连接态停广播,断开恢复
- 5 分钟总超时无连接 → 重启回正常运行

### GATT 服务(自定义 128-bit UUID)

| 特征 | 属性 | 用途 |
|------|------|------|
| `handshake` | Write/Notify | ECDH 公钥交换 |
| `scan_result` | Read/Notify | WiFi 扫描结果,分片下发 |
| `config_write` | Write | AES-GCM 加密配置 JSON,分片上传 |
| `status` | Read/Notify | `ok` / 校验错误文案 / `rebooting` |
| `command` | Write | `scan` / `save` / `reboot` |

### 分片协议

微信小程序单次 BLE 写入默认 20 字节(`wx.setBLEMTU` 仅安卓有效)。配置 JSON
轻松超 100 字节,需自定义分片:

```
每片头 4 字节:[2B 总长][1B 序号][1B 标志]
设备端缓冲重组,收齐后整体解析
scan_result 下发同理分片 + Notify
```

### 会话状态机

```
IDLE → HANDSHAKE → READY → RECEIVING → VALIDATING → SAVED → REBOOT
         校验失败回 READY 让小程序重试
         超时 / 断开回 IDLE
```

同一时刻只接受一个配网连接;会话超时(60s 无进展)自动断开。

### 安全

- 出厂烧录每台设备一个随机 PSK,存 `config.json` 字段 `ble_psk`
- 小程序后台按 deviceId 存 PSK
- 握手:ECDH(Curve25519)协商会话密钥 + PSK 做 HMAC 身份认证
- `config_write` 走 AES-GCM 加密
- `mbedtls` 随 TLS 已链入,Curve25519/AES-GCM/HMAC 复用,不额外占多少 flash

### 内存

会话开始时断开 MQTT、释放 TLS 上下文(复用现有 OTA 的内存释放模式),避免
TLS + BLE GATT + ECDH 缓冲同时占用导致 OOM。

### 落盘

- 校验 + 落盘抽成公共函数 `applyAndSaveConfig()`(原 `handleAPSave` 的逻辑)
- 若改了 `mqtt_server`,沿用「切换前清除旧 broker retained 消息」逻辑
- 保存后 `ESP.restart()`

## 6. 代码结构变更

| 项 | 操作 |
|----|------|
| 全文件 | 移除 `#ifdef ESP8266` / `#ifndef ESP8266` 分支,删 ESP8266 头文件 |
| SoftAP | 删除 `enterSoftAPMode` / `handleAP*` / `checkSoftAPFlag` / `CONFIG_HTML` / WebServer / DNSServer |
| 公共函数 | 抽 `applyAndSaveConfig()`(校验 + 落盘) |
| 新增 | `bleProvBegin` / GATT 回调 / 分片重组 / ECDH+AES / 会话状态机 |
| `DevConfig` | 加 `ble_psk` 字段;移除 `ap_full_config`;`config.h` 版本号 bump |
| 串口 CLI | 去掉 10s 按键手势,保留 `config` 入口;`help` 文本精简 |
| 文档 | `PROTOCOL.md` 加 BLE GATT + 分片章节;`CLAUDE.md` 重写 |
| 依赖 | 新增 `NimBLE-Arduino`(h2zero) |

## 7. 落地前门槛验证

1. **固件体积**:加 NimBLE 后编译,确认 < 1.9MB 分区
2. **RAM 峰值**:实测 BLE 会话 + ECDH 期间堆余量(MQTT 已断开前提下)

## 8. 分阶段实现

1. ~~**阶段 0**:移除 ESP8266 兼容代码,固件成为 ESP32-C3 独占~~ ✅
2. ~~**阶段 1**:明文 GATT + 分片协议~~ ✅
3. ~~**阶段 2**:X25519 ECDH + AES-256-GCM + per-device PSK~~ ✅
4. **阶段 3**:微信小程序联调,完善 LED 反馈与超时兜底

> 结构收尾(移除 SoftAP、按键改 BLE、首次开机自动进 BLE)已随阶段 1/2 完成。

### 阶段 2 最终实现细节

- **PSK**:首次开机随机生成 16B,存 `/ble_psk.bin`,工厂重置不删除;BLE 模式
  启动时串口打印 hex。
- **握手**:`handshake` 特征(`e0c1a705`),客户端写 32B X25519 公钥,设备
  回传 32B 公钥(notify)。固件用 mbedtls ECDH(Curve25519/Everest),
  网页用 Web Crypto `X25519`。
- **会话密钥**:`HMAC-SHA256(key=PSK, msg=shared ‖ "wol-ble-v1")`,32B 即
  AES-256 密钥。
- **config 加密**:载荷 = `iv(12) ‖ tag(16) ‖ 密文`,AES-256-GCM,无 AAD;
  Web Crypto 输出为 `密文‖tag`,客户端需重排为 `iv‖tag‖密文`。
- 设备未完成握手时拒收 config;解密验签失败回 `error:` 状态。
- `scan` / `status` 仍为明文(SSID、状态文本非敏感)。

### 固件收尾(随阶段 2 复查完成)✅

- BLE 模式超时改为绝对计时:仅对**已有配置**的设备生效(无配置设备常驻配网,
  重启也只会再进本模式);修掉了「连上又中途放弃 → 永久卡死」的 bug。
- LED:未连接 500ms 慢闪,客户端已连接 150ms 快闪,配置成功常亮。

## 9. 阶段 3 待办:微信小程序

> 设备/固件侧已完工,阶段 3 是纯客户端工作。`test/ble_test.html` 是完整的
> 参考实现,小程序逻辑可照搬。

### 前置条件

- 微信小程序 AppID(微信公众平台注册,个人可注册)
- 微信开发者工具(Mac 端)
- 一台真手机:**BLE 在模拟器中不可用**,必须真机调试

### 关键技术点

- **加密库**:小程序无 Web Crypto,X25519 与 AES-256-GCM 需打包纯 JS 库。
  推荐 `@noble/curves`(x25519)+ `@noble/ciphers`(aes-gcm),或 tweetnacl。
- **BLE API**:`wx.openBluetoothAdapter` / `startBluetoothDevicesDiscovery` /
  `createBLEConnection` / `getBLEDeviceServices` / `getBLEDeviceCharacteristics` /
  `writeBLECharacteristicValue` / `notifyBLECharacteristicValueChange` /
  `onBLECharacteristicValueChange`。
- **MTU**:`wx.setBLEMTU` 仅安卓有效,iOS 自动协商;分片协议照搬测试页。
- **PSK 获取**:`wx.scanCode` 扫设备二维码,解析 URL query(格式见下)。

### 功能流程

扫二维码取 PSK → 扫描 BLE 设备 → 连接 `WoL-XXXX` → X25519 握手 →
触发 WiFi 扫描 → 填表单 → AES-GCM 加密下发 → 显示 status 结果。

### 可选(后续)

- 小程序后台维护各设备最新 MQTT 凭据,换服务器时一键下发。
- 工程位置:`miniprogram/` 子目录或独立仓库(待定)。

## 10. 二维码格式契约

设备贴二维码(底部/内侧标签),配网 App 扫码获取设备身份与 PSK。

**格式:URL(非 JSON)**

```
https://i.iot-c.top/p?m=W1&v=1&id=wol-a1b2c3d4e5f6&psk=<32 位十六进制>
```

| 参数 | 含义 |
|------|------|
| `m`  | 产品型号(`PRODUCT_MODEL`,如 `W1`),兼类型守卫:App 扫码先校验 |
| `v`  | 二维码格式版本,便于后续演进 |
| `id` | 设备 mqtt_id(`wol-<12 位 MAC 十六进制>`),用于匹配 BLE 设备名 `WoL-XXXX` |
| `psk`| 16 字节 PSK 的 32 位十六进制 |

**为何用 URL 而非 JSON**:URL 是 JSON 的超集 ——

- 今天:小程序内 `wx.scanCode` 拿到字符串,解析 query 即可,零基建
- 以后:域名 `i.iot-c.top` 完成 ICP 备案 + 后台配「扫普通链接二维码打开小程序」
  规则后,**同一批二维码**被微信冷扫即可拉起小程序,无需重印标签
- 即使冷扫未配好,落地页也能引导用户「请用小程序扫码」

**安全性**:配网模式按键门控 + PSK 不过空气,二维码方案对家用场景安全
(详见 §6)。`psk` 在 URL query 中;落地页应做成纯静态、不记日志。

**产线流程**:固件首次开机自生成 PSK → 采集工具读串口 `[ID]` 机读行
(`[ID] model=W1 mqtt_id=... psk=...`)→ 套 URL 模板生成二维码 → 打印贴标。
