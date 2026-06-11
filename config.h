#pragma once

// ── 硬件（配置触发引脚） ───────────────────────────────────────────────────────
// 按住 3s 松手：写 /bleprov 标志并重启进入 BLE 配网模式（配置保留）
// 按住 10s：工厂重置（清空配置）后重启进入 BLE 配网模式
#define CFG_RESET_PIN     9       // ESP32-C3 BOOT 键，active LOW

// ── WiFi ─────────────────────────────────────────────────────────────────────
#define WIFI_TIMEOUT_MS          (60UL * 1000)  // 60s：WiFi 掉线后等待 SDK 自动重连的超时，超时重启
#define WIFI_PER_NETWORK_TIMEOUT_MS (15UL * 1000) // 15s：多网络轮试时每个网络的连接超时
// 注意：ESP32-C3 会在连接前主动扫描，优先尝试当前可见的网络。
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

// ── 局域网发现 net_scan ───────────────────────────────────────────────────────
#define NETSCAN_ARP_WINDOW      8     // 每窗 ARP 请求数（lwIP ARP 表默认 10 槽，留 2 余量）
#define NETSCAN_HARVEST_MS      200   // 每窗发送后等待应答收割的时间（ms）
#define NETSCAN_MAX_HOSTS       64    // 结果上限（静态数组，单条 ~28B，共 <2KB）
#define NETSCAN_NAME_TIMEOUT_MS 300   // 单台 NBNS / mDNS 名字查询超时（ms）
#define NETSCAN_BATCH           5     // 每批上报主机数（控制 MQTT 报文 <1KB）

// ── 唤醒验证 wake verify（wol verify:true）─────────────────────────────────────
#define WAKE_VERIFY_TIMEOUT_MS  (60UL * 1000) // 探测目标上线的总超时
#define WAKE_VERIFY_PROBE_MS    1000          // 探测节拍：每秒一轮
#define WAKE_VERIFY_WINDOW      8             // 无 IP 提示时每轮 ARP 请求数（轮转扫 /24）

// ── 测速 ─────────────────────────────────────────────────────────────────────
#define SPEEDTEST_DEFAULT_URL   "http://mirrors.tuna.tsinghua.edu.cn/speedtest/100mb.bin"
#define SPEEDTEST_DEFAULT_SECS  15     // 秒，默认测速时长上限
#define SPEEDTEST_MIN_HEAP      6144   // 触发测速前要求的最大连续 heap（字节）

// ── 产品标识与版本 ────────────────────────────────────────────────────────────
#define PRODUCT_MODEL  "W1"               // 产品型号（随二维码 / [ID] 机读行上报）
#define FW_VERSION     "3.0.0-20260518"
