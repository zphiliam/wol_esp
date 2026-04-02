# TODO

## ✅ 配置持久化改造（v2.0.0-20260328）

凭据从编译期 `config.h` 移到运行时 LittleFS `/config.json`，固件本身不含敏感信息。

- 凭据（WiFi、MQTT、WoL MAC）存于 LittleFS `/config.json`
- 串口 CLI 写入配置（`set` / `show` / `save` / `reset` / `help`）
- 无配置文件时 LED 100ms 快闪，等待串口写入
- WiFi 首次连接超时（60s）自动进入串口 CLI；曾连接成功后超时走重启逻辑
- `wifi_ever_ok` 持久化于 `/config.json`，控制超时策略

---

## ✅ 运行时进入配置模式（v2.1.x）

任意阶段均可触发，无需重新烧录。

- 串口输入 `config` + 回车：写 `/reconfig` 标志 → 重启 → 串口 CLI（保留现有配置）
- 按住 FLASH/BOOT 键 3s 后松手：写 `/softap` 标志 → 重启 → SoftAP 网页配置
- 按住 FLASH/BOOT 键 10s：写 `/reconfig` 标志 → 重启 → 串口 CLI（配置保留）
- LED 双闪提示 3s 阈值，五闪确认 10s 触发
- 移除了上电时的 `checkFactoryReset()`，解决 ESP32-C3 上电按键与 ROM 下载模式冲突

---

## ✅ LED 非阻塞化（v2.1.4）

- 用状态机替换阻塞式 `delay()` 闪烁，`loop()` 始终保持响应

---

## ✅ SoftAP 网页配置模式（v2.2.0）

面向普通用户的图形化配网，无需串口工具。

- AP SSID：`WoL-Setup-XXXX`（chip ID 末 4 位），开放网络，IP `192.168.4.1`
- 网页扫描并选择 WiFi SSID，填写密码和 WoL MAC 后保存重启
- `ap_full_config=true` 时额外显示完整 MQTT 字段
- 串口输入 `ap` + 回车可随时切换到 SoftAP 模式
- HTML 抽离到 `ap_html.h`（PROGMEM），节省堆内存

---

## ✅ 密码显示切换 & 串口 reboot 命令（v2.2.1）

- SoftAP 配置页密码输入框右侧新增眼睛按钮，可切换明文/密文显示
- 运行时串口输入 `reboot` + 回车直接重启设备
- `wol` 指令支持可选 `mac` 参数，不填则使用配置中的目标 MAC

---

## 待规划

- OTA 固件升级
