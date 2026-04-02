#pragma once

// ── 硬件（配置触发引脚） ───────────────────────────────────────────────────────
// 按住 3s 松手：写 /softap 标志并重启进入 SoftAP 模式
// 按住 10s：写 /reconfig 标志并重启进入配置模式
#if defined(ESP8266)
#  define CFG_RESET_PIN     0       // FLASH 键，active LOW
#else
#  define CFG_RESET_PIN     9       // BOOT 键，active LOW
#endif

// ── WiFi ─────────────────────────────────────────────────────────────────────
#define WIFI_TIMEOUT_MS          (60UL * 1000)  // 60s：曾连接成功后的超时上限，超时重启；设长是为了避免路由器重启期间设备陷入快速重启循环
#define WIFI_TIMEOUT_FIRSTRUN_MS (180UL * 1000) // 180s：首次连接超时，进入串口配置模式；手机热点激活较慢，需要足够等待时间
// 注意：ESP32 上首次连接前会主动扫描 SSID，若不存在则立即进入配置模式。
//       隐藏网络（不广播 SSID）不会出现在扫描结果中，会被误判为不存在而直接进入配置模式，无法正常连接。

// ── MQTT ─────────────────────────────────────────────────────────────────────
#define CONF_MQTT_KEEPALIVE  120    // 秒，TCP 层保活间隔（broker 超时则断开）
#define MQTT_SOCK_TIMEOUT    5      // 秒，socket 读超时（防卡死关键）
#define MQTT_RETRY_MAX       5      // 重连失败超过此次数就重启

// ── 行为参数 ──────────────────────────────────────────────────────────────────
#define REBOOT_INTERVAL_MS       (24UL * 60 * 60 * 1000)  // 24h 定时重启
#define STATUS_INTERVAL_DEFAULT  300    // 秒，默认 status 上报间隔（5 分钟）
#define STATUS_INTERVAL_MIN      10     // 秒，最短间隔
#define STATUS_INTERVAL_MAX      3600   // 秒，最长间隔（1 小时）

// ── 版本 ──────────────────────────────────────────────────────────────────────
#define FW_VERSION  "2.3.1-20260402"
