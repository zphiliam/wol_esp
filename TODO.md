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

## ⏳ 进行中 / 待启动

### 阶段 3：微信小程序配网（待启动）

- 纯客户端工作，照搬 `test/ble_test.html` 逻辑
- 加密库需打包纯 JS（`@noble/curves` + `@noble/ciphers`），BLE 须真机调试
- 工程位置（`miniprogram/` 子目录 vs 独立仓库）待定
- 详见 `docs/BLE_REDESIGN.md` §9

### v3 服务端（设计定稿，待实现）

- 自建 EMQX + 后端中转 + claim 覆盖式重绑，含 SQLite DDL
- 尚无实现代码；建议先落后端骨架（认证端点 + claim + 设备登记导入）
- 详见 `docs/ARCHITECTURE_v3.md`
