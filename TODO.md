# TODO

> 进度的权威记录在 `docs/BLE_REDESIGN.md`（配网阶段）与 `docs/ARCHITECTURE_v3.md`
> （服务端）。本文件只留里程碑速览。

## ✅ 已完成

### 配置持久化改造（v2.0.0-20260328）

凭据从编译期 `config.h` 移到运行时 LittleFS `/config.json`，固件本身不含敏感信息。

- 凭据（WiFi、MQTT、WoL MAC）存于 LittleFS `/config.json`
- 串口 CLI 写入配置（`set` / `show` / `save` / `reset` / `help`）
- WiFi 首次连接超时自动进配网；曾连接成功后超时走重启逻辑
- `wifi_ever_ok` 持久化，控制超时策略

### 多网络历史（v2.1.x）

- `/wifi_networks.json` 存最多 5 条历史 WiFi，头部为最近连接网络
- 启动时扫描可见网络排序后轮试；连接成功后移至列表头部并持久化

### LED 非阻塞化（v2.1.4）

- 用状态机替换阻塞式 `delay()` 闪烁，`loop()` 始终保持响应

### OTA 固件升级（v2.3.x）

- MQTT 下发 `{"cmd":"ota","url":"..."}`，流式下载写 Flash，自动跟踪 HTTP 重定向
- 发布 `ota_start` / `ota_success` / `ota_fail` 事件

### v3.0 — ESP32-C3 独占 + BLE 配网（FW 3.0.0-20260518）

- 移除 ESP8266 兼容代码与 SoftAP 网页配网，配网统一走低功耗蓝牙（BLE）
- 阶段 1：NimBLE GATT + 分片协议（明文）
- 阶段 2：X25519 ECDH + AES-256-GCM + per-device PSK
- 固件收尾：按键触发 BLE、首次开机自动进 BLE、超时改 20min 无活动、握手修复
- `mqtt_id` 改为芯片 eFuse MAC 自动派生；串口 `id` 命令（含机读单行）
- `wol_mac` 改为可选（手机端多目标管理，`wol` 指令随包带 `mac`）
- 产品型号 `W1` + 二维码 URL 契约；产线身份采集工具 `scripts/collect_id.py`
- 详见 `docs/BLE_REDESIGN.md`

---

### 添加电脑体验：局域网发现 net_scan（阶段 1，固件）

- MQTT `{"cmd":"net_scan"}` / 串口 `netscan`：ARP 扫 /24 + NBNS/mDNS 解析主机名，分批上报
- 非阻塞状态机，直读 lwIP ARP 表；事件 `net_scan_start` / `net_scan_result`
- 阶段 2（wol `verify` 唤醒验证）及后端/小程序阶段见 `docs/NET_SCAN.md`

---

### 阶段 3：微信小程序（独立仓库 `../wolapp`）

- BLE 加密配网（含 MQTT 凭据下发）、扫描发现 + 测试唤醒添加电脑均已实现
- 详见 `docs/BLE_REDESIGN.md` §9

### v3 服务端（独立仓库 `../esp_auth`，已部署）

- Go + SQLite：EMQX HTTP 认证钩子、claim 覆盖式重绑、指令中转与设备状态
- net_scan / 唤醒验证后端（task 轮询 + OUI 富化 + last_ip）
- 详见 `docs/ARCHITECTURE_v3.md`

---

## ⏳ 进行中 / 待启动

### net_scan 阶段 3 收尾（非阻塞）

- 存量 wol/cmd 接口迁移 task 模型 + SSE 下线（后端）
- 全链路真机联调
- 详见 `docs/NET_SCAN.md`
