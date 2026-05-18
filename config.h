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

// ── 测速 ─────────────────────────────────────────────────────────────────────
#define SPEEDTEST_DEFAULT_URL   "http://mirrors.tuna.tsinghua.edu.cn/speedtest/100mb.bin"
#define SPEEDTEST_DEFAULT_SECS  15     // 秒，默认测速时长上限
#define SPEEDTEST_MIN_HEAP      6144   // 触发测速前要求的最大连续 heap（字节）

// ── 路由器重启 ────────────────────────────────────────────────────────────────
// 目标为一台当交换机用的 ~2010 年代国产白牌旧路由器，采用 Broadcom 系 SoC，
// 固件为 "WEB SDK" 型公版 Web GUI（wdk.js / cdb 配置数据库 / product.js）。
//   · HTTP 服务：HTTP/1.0，Server: "HTTP Server"，HTTP Basic 认证 realm="Router"
//   · 所有动态操作经 cli.cgi?cmd=<CLI命令>；CLI 含 wan/status/save/commit/upgrade/
//     ping/arp/route/ifcfg/time/echo/mdio/mactbl 等（mdio/mactbl 即内置交换机诊断）
//   · cmd=reboot 触发重启并返回 HTTP 200（未列入 help，但实测可用）
//   · 探测到固件 $sw_ver=2.0 p40 / $hw_ver=1.0.2.0 / 引导版本 $hw_bver=0.0.9.4
#define ROUTER_REBOOT_PATH      "cli.cgi?cmd=reboot"  // 默认重启接口路径（不含前导 /）
#define ROUTER_HTTP_TIMEOUT_MS  10000                 // 毫秒，路由器 HTTP 请求超时

// ── 版本 ──────────────────────────────────────────────────────────────────────
#define FW_VERSION  "2.5.0-20260518"
