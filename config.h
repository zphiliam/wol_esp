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
#define WIFI_TIMEOUT_MS          (60UL * 1000)  // 60s：WiFi 掉线后等待 SDK 自动重连的超时，超时重启
#define WIFI_PER_NETWORK_TIMEOUT_MS (15UL * 1000) // 15s：多网络轮试时每个网络的连接超时
// 注意：ESP32-C3 会在连接前主动扫描，优先尝试当前可见的网络；ESP8266 按列表顺序盲试。
//       隐藏网络（不广播 SSID）不出现在扫描结果中，会排在候选列表最后尝试。

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
#define FW_VERSION  "2.4.1-20260408"
