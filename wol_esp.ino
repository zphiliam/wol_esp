/**
 * WoL ESP32-C3 — C++ Arduino 版  v3.0
 *
 * 功能：通过 MQTT 远程唤醒局域网内电脑
 * 硬件：ESP32-C3 SuperMini（LED GPIO8，active LOW）
 *
 * 依赖库（Arduino IDE 库管理器安装）：
 *   - PubSubClient    by Nick O'Leary
 *   - ArduinoJson     by Benoit Blanchon
 *   - NimBLE-Arduino  by h2zero
 *
 * 配置：凭据存于 LittleFS /config.json，固件本身不含敏感信息
 *   首次无配置                   → 自动进入 BLE 配网模式
 *   运行时串口输入 "ble" + 回车  → 写 /bleprov → 重启 → BLE 配网
 *   运行时按住 BOOT 键 3s 松手   → 写 /bleprov → 重启 → BLE 配网（配置保留）
 *   运行时按住 BOOT 键 10s       → 工厂重置（清空配置）→ 重启 → BLE 配网
 *   串口输入 "config"（隐藏恢复通道）→ 写 /reconfig → 重启 → 串口 CLI
 *   BLE 设备名 WoL-XXXX；GATT 协议见 docs/BLE_REDESIGN.md
 *
 * 主题（运行时由 mqtt_id 拼接）：
 *   下行指令  home/wol/{mqtt_id}/cmd
 *   上行事件  home/wol/{mqtt_id}/event
 *
 * 指令列表：
 *   {"cmd":"wol"}                              唤醒电脑
 *   {"cmd":"wol","mac":"...","verify":true,"ip":"..."}  唤醒并验证目标是否上线（回报 wake_result）
 *   {"cmd":"ping"}                             测试连通性
 *   {"cmd":"reboot"}                           重启设备
 *   {"cmd":"info"}                             查询设备运行状态（version/ssid/rssi/wol_mac/reconnects）
 *   {"cmd":"config"}                           查询完整配置（含 wifi_networks 列表）
 *   {"cmd":"led","val":"on|off|toggle|query"}  控制 LED
 *   {"cmd":"led","val":"blink","times":5,"interval":500}
 *   {"cmd":"set","key":"status_interval","val":30}  设置状态上报间隔（秒，0=禁用）
 *   {"cmd":"set_mqtt","server":"...","port":8883,"user":"...","pass":"..."}  在线换 broker（成功后重启）
 *   {"cmd":"ota","url":"https://..."}          OTA 升级（自动跟踪重定向，支持 GitHub release URL）
 *   {"cmd":"speedtest"}                         网络测速（HTTP 下载丢弃，默认 15s，MQTT 保持连接）
 *   {"cmd":"speedtest","url":"http://...","max_seconds":10}
 *   {"cmd":"net_scan"}                          局域网发现（ARP 扫描 + NBNS/mDNS，分批上报）
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <WiFiUdp.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include "config.h"

// ── OTA ──────────────────────────────────────────────────────────────────────
#include <HTTPClient.h>
#include <Update.h>

// ── BLE 配网（NimBLE-Arduino） ────────────────────────────────────────────────
#include <NimBLEDevice.h>

// ── BLE 配网加密（mbedtls，ESP32 内置） ───────────────────────────────────────
#include <esp_random.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>

#include "esp_wifi.h"   // esp_wifi_set_max_tx_power()
#include "esp_mac.h"    // esp_read_mac()

// ── 局域网发现 net_scan（lwIP ARP 表直读 + NBNS/mDNS 名字查询） ────────────────
#include "lwip/etharp.h"   // etharp_request() / etharp_find_addr()
#include "lwip/netif.h"    // netif_list / netif_ip4_addr()
#include "lwip/tcpip.h"    // LOCK_TCPIP_CORE() / UNLOCK_TCPIP_CORE()

// ── 硬件 ──────────────────────────────────────────────────────────────────────
const uint8_t LED_PIN = 8;   // ESP32-C3 SuperMini，active LOW

// ── 运行时配置结构（从 LittleFS /config.json 加载） ────────────────────────────
struct DevConfig {
    char     wifi_ssid[33];  // 802.11 规范 SSID 最长 32 字节 + '\0'
    char     wifi_pass[64];  // WPA2 密码最长 63 字节 + '\0'
    char     mqtt_server[64];
    uint16_t mqtt_port;
    char     mqtt_user[64];  // 部分 broker 用户名较长，64 字节留足余量
    char     mqtt_pass[64];  // 同上，密码统一扩至 64 字节
    char     mqtt_id[32];
    char     wol_mac[13];    // 12 位十六进制 + '\0'
    bool     wifi_ever_ok;   // 是否曾经成功连接过 WiFi（决定超时策略）
    int8_t   wifi_tx_power;  // WiFi 发射功率（dBm，2-20）；0 表示使用平台默认值
} cfg;

// ── WiFi 历史网络 LRU 缓存（固定池双向链表）─────────────────────────────────
// 头部 = 最近使用，尾部 = 最久未用（满时淘汰）
// int8_t 存 prev/next（N≤5，范围 -1~4 完全够用，每节点仅多 3 字节开销）
#define WIFI_MAX_NETWORKS 5
struct WifiNode {
    char   ssid[33];
    char   pass[64];
    int8_t prev;   // 链表中上一节点在池中的索引，-1 = 无
    int8_t next;   // 链表中下一节点在池中的索引，-1 = 无
    bool   used;   // 是否被占用
};
static WifiNode wifiPool[WIFI_MAX_NETWORKS];
static int8_t   wifiHead  = -1;  // 最近使用端（索引）
static int8_t   wifiTail  = -1;  // 最久未用端（索引）
static uint8_t  wifiCount =  0;

// ── MQTT 主题（运行时由 cfg.mqtt_id 填充） ─────────────────────────────────────
char TOPIC_SUB[64];
char TOPIC_PUB[64];

// ── 全局对象 ──────────────────────────────────────────────────────────────────
WiFiClientSecure wifiClient;
PubSubClient     mqtt(wifiClient);
WiFiUDP          udp;

// ── 运行时可修改的配置 ────────────────────────────────────────────────────────
unsigned long statusIntervalMs = (unsigned long)STATUS_INTERVAL_DEFAULT * 1000;

// ── 计时器 ────────────────────────────────────────────────────────────────────
unsigned long lastStatusMs = 0;

// ── MQTT 连接状态 ─────────────────────────────────────────────────────────────
bool     mqttEverConnected  = false;
uint16_t mqttReconnectCount = 0;
unsigned long wifiConnectMs = 0;   // WiFi 首次连接耗时（ms），写入 online 报文

// ── 运行时触发配置模式 ────────────────────────────────────────────────────────
String        serialLineBuf;           // 串口输入行缓冲
unsigned long btnPressStart = 0;       // 按键按下时刻
bool          btnPressing   = false;   // 按键是否持续按住中
bool          btnHit3s      = false;   // 已达 3s 阈值（松手触发 BLE 配网）

// ── 局域网发现 net_scan 类型（定义在前，供 .ino 自动生成的函数原型可见） ───────
enum NetScanState {
    NS_IDLE,
    NS_ARP_SEND,      // 发一窗 ARP 请求
    NS_ARP_HARVEST,   // 等待后从 lwIP ARP 表收割
    NS_NAME_QUERY,    // 逐台查询主机名
    NS_REPORT,        // 分批发布结果事件
};

struct ScanHost {
    uint32_t ip;        // lwIP 网络字节序（与 WiFi.localIP() 的 uint32 表示一致）
    uint8_t  mac[6];
    char     name[16];  // 主机名，15 字符 + NUL
    uint8_t  src;       // 0=无名 1=nbns 2=mdns
};

// ── LED 非阻塞闪烁状态 ────────────────────────────────────────────────────────
struct BlinkJob {
    bool          active;        // 是否正在执行
    int           remaining;     // 剩余翻转次数（times × 2）
    int           intervalMs;    // 每次翻转间隔（ms）
    unsigned long nextToggleMs;  // 下次翻转的绝对时间戳
} blink = {};

// ─────────────────────────────────────────────────────────────────────────────
// saveConfig / loadConfig
// ─────────────────────────────────────────────────────────────────────────────
bool saveConfig() {
    StaticJsonDocument<768> doc;
    doc["wifi_ssid"]    = cfg.wifi_ssid;
    doc["wifi_pass"]    = cfg.wifi_pass;
    doc["mqtt_server"]  = cfg.mqtt_server;
    doc["mqtt_port"]    = cfg.mqtt_port;
    doc["mqtt_user"]    = cfg.mqtt_user;
    doc["mqtt_pass"]    = cfg.mqtt_pass;
    doc["wol_mac"]       = cfg.wol_mac;
    doc["wifi_ever_ok"]  = cfg.wifi_ever_ok;
    doc["wifi_tx_power"]   = cfg.wifi_tx_power;
    File f = LittleFS.open("/config.json", "w");
    if (!f) return false;
    serializeJson(doc, f);
    f.close();
    return true;
}

// 返回 true 表示配置完整可用
bool loadConfig() {
    if (!LittleFS.exists("/config.json")) return false;
    File f = LittleFS.open("/config.json", "r");
    if (!f) return false;
    StaticJsonDocument<768> doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err != DeserializationError::Ok) return false;

    strlcpy(cfg.wifi_ssid,   doc["wifi_ssid"]   | "", sizeof(cfg.wifi_ssid));
    strlcpy(cfg.wifi_pass,   doc["wifi_pass"]    | "", sizeof(cfg.wifi_pass));
    strlcpy(cfg.mqtt_server, doc["mqtt_server"]  | "", sizeof(cfg.mqtt_server));
    cfg.mqtt_port = doc["mqtt_port"] | 8883;
    strlcpy(cfg.mqtt_user,   doc["mqtt_user"]    | "", sizeof(cfg.mqtt_user));
    strlcpy(cfg.mqtt_pass,   doc["mqtt_pass"]    | "", sizeof(cfg.mqtt_pass));
    strlcpy(cfg.wol_mac,     doc["wol_mac"]      | "", sizeof(cfg.wol_mac));
    cfg.wifi_ever_ok   = doc["wifi_ever_ok"]   | false;
    // 默认值：ESP32-C3 SuperMini 天线设计缺陷+LDO电流不足，15dBm 最稳定（ESP3D 实测推荐）
    cfg.wifi_tx_power  = doc["wifi_tx_power"]  | 15;

    // 必填项校验（wifi_ssid 已迁移至 wifi_networks.json；mqtt_id 由芯片 ID 派生；
    // wol_mac 改为可选——用户在手机端管理多台目标 PC，下发指令时随包带 mac）
    return strlen(cfg.mqtt_server) > 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// 设备身份：mqtt_id 与 BLE PSK
// ─────────────────────────────────────────────────────────────────────────────
#define BLE_PSK_LEN   16
#define BLE_PSK_FILE  "/ble_psk.bin"
static uint8_t blePsk[BLE_PSK_LEN];   // per-device PSK（设备身份，工厂重置不删）

// mqtt_id 由芯片 eFuse MAC 派生：全局唯一、重启稳定，无需用户配置。
// 同时用作 MQTT client ID 与主题中的设备标识。
static void genMqttId(char* buf, size_t len) {
    snprintf(buf, len, "wol-%012llx", (unsigned long long)ESP.getEfuseMac());
}

// 打印设备身份（mqtt_id + BLE PSK）：人读框 + 机读单行 [ID]。
// 供串口 CLI、BLE 配网模式、正常运行三处的 id 命令共用；机读行便于采集工具解析。
static void printDeviceId() {
    // 芯片出厂 WiFi STA MAC（路由器/DHCP 可见的标准 MAC）。
    // 与 mqtt_id 同源自 eFuse，但 mqtt_id 的 hex 是字节反序，故单独输出真实 MAC。
    // esp_read_mac 不依赖 WiFi 初始化，三种模式下均可调用。
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    Serial.println(F("------ device identity ------"));
    Serial.printf("  model   : %s\n", PRODUCT_MODEL);
    Serial.printf("  mqtt_id : %s\n", cfg.mqtt_id);
    Serial.printf("  mac     : %02X:%02X:%02X:%02X:%02X:%02X\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    Serial.print(F("  ble_psk : "));
    for (int i = 0; i < BLE_PSK_LEN; i++) Serial.printf("%02x", blePsk[i]);
    Serial.println();
    Serial.println(F("-----------------------------"));
    // 机读单行：[ID] model=... mqtt_id=... mac=... psk=...
    Serial.printf("[ID] model=%s mqtt_id=%s mac=%02X%02X%02X%02X%02X%02X psk=",
                  PRODUCT_MODEL, cfg.mqtt_id,
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    for (int i = 0; i < BLE_PSK_LEN; i++) Serial.printf("%02x", blePsk[i]);
    Serial.println();
}

// ─────────────────────────────────────────────────────────────────────────────
// LRU WiFi 历史缓存：内部工具 + 公共接口
// 持久化到 /wifi_networks.json，格式不变（兼容旧固件）
// ─────────────────────────────────────────────────────────────────────────────

// 从池中找空闲槽（count < MAX 时必然存在）
static int8_t lruAlloc() {
    for (int8_t i = 0; i < WIFI_MAX_NETWORKS; i++)
        if (!wifiPool[i].used) return i;
    return -1;
}

// 按 SSID 查找节点索引，不存在返回 -1
static int8_t lruFind(const char* ssid) {
    for (int8_t cur = wifiHead; cur != -1; cur = wifiPool[cur].next)
        if (strcmp(wifiPool[cur].ssid, ssid) == 0) return cur;
    return -1;
}

// 将节点从链表中摘下（不释放槽位）
static void lruDetach(int8_t i) {
    int8_t p = wifiPool[i].prev, n = wifiPool[i].next;
    if (p != -1) wifiPool[p].next = n; else wifiHead = n;
    if (n != -1) wifiPool[n].prev = p; else wifiTail = p;
    wifiPool[i].prev = wifiPool[i].next = -1;
}

// 将节点插到链表头部
static void lruPushHead(int8_t i) {
    wifiPool[i].prev = -1;
    wifiPool[i].next = wifiHead;
    if (wifiHead != -1) wifiPool[wifiHead].prev = i;
    wifiHead = i;
    if (wifiTail == -1) wifiTail = i;
}

// 添加或更新：已存在则更新密码并移到头部；已满则淘汰尾部后插入头部
void lruAddOrUpdate(const char* ssid, const char* pass) {
    if (!ssid || ssid[0] == '\0') return;

    int8_t idx = lruFind(ssid);
    if (idx != -1) {
        strlcpy(wifiPool[idx].pass, pass, sizeof(wifiPool[0].pass));
        if (idx != wifiHead) { lruDetach(idx); lruPushHead(idx); }
        return;
    }
    if (wifiCount >= WIFI_MAX_NETWORKS) {
        int8_t evict = wifiTail;
        lruDetach(evict);
        wifiPool[evict].used = false;
        wifiCount--;
    }
    idx = lruAlloc();
    strlcpy(wifiPool[idx].ssid, ssid, sizeof(wifiPool[0].ssid));
    strlcpy(wifiPool[idx].pass, pass, sizeof(wifiPool[0].pass));
    wifiPool[idx].prev = wifiPool[idx].next = -1;
    wifiPool[idx].used = true;
    lruPushHead(idx);
    wifiCount++;
}

// 按顺序位置删除（pos=0 为头部/最近），返回是否成功
bool lruRemoveAt(uint8_t pos) {
    int8_t cur = wifiHead;
    for (uint8_t i = 0; i < pos && cur != -1; i++) cur = wifiPool[cur].next;
    if (cur == -1) return false;
    lruDetach(cur);
    wifiPool[cur].used = false;
    wifiCount--;
    return true;
}

// 按顺序位置读取（pos=0 = 最近），越界返回 nullptr
const WifiNode* lruGet(uint8_t pos) {
    int8_t cur = wifiHead;
    for (uint8_t i = 0; i < pos && cur != -1; i++) cur = wifiPool[cur].next;
    return (cur != -1) ? &wifiPool[cur] : nullptr;
}

// 重置缓存（清除所有节点）
void lruInit() {
    memset(wifiPool, 0, sizeof(wifiPool));
    wifiHead = wifiTail = -1;
    wifiCount = 0;
}

void saveWifiNetworks() {
    StaticJsonDocument<768> doc;
    JsonArray arr = doc.to<JsonArray>();
    for (uint8_t i = 0; i < wifiCount; i++) {
        const WifiNode* n = lruGet(i);
        JsonObject obj = arr.createNestedObject();
        obj["ssid"] = n->ssid;
        obj["pass"] = n->pass;
    }
    File f = LittleFS.open("/wifi_networks.json", "w");
    if (!f) { Serial.println(F("[WiFi] saveWifiNetworks: open failed")); return; }
    serializeJson(doc, f);
    f.close();
    Serial.printf("[WiFi] saved %d network(s) to wifi_networks.json\n", wifiCount);
}

// 返回 true 表示至少有一条可用网络
bool loadWifiNetworks() {
    lruInit();
    if (LittleFS.exists("/wifi_networks.json")) {
        File f = LittleFS.open("/wifi_networks.json", "r");
        if (f) {
            StaticJsonDocument<768> doc;
            DeserializationError err = deserializeJson(doc, f);
            f.close();
            if (err == DeserializationError::Ok && doc.is<JsonArray>()) {
                // 文件头部 = 最近使用，逆序插入使头部保持顺序
                // lruAddOrUpdate 自带去重：若 SSID 已存在则更新密码并移头，不会产生重复节点
                JsonArray arr = doc.as<JsonArray>();
                int total = arr.size();
                for (int i = total - 1; i >= 0; i--) {
                    JsonObject obj = arr[i];
                    const char* ssid = obj["ssid"] | "";
                    if (ssid[0] == '\0') continue;
                    lruAddOrUpdate(ssid, obj["pass"] | "");
                }
                // 若文件里有重复条目（旧固件遗留），加载后条数会减少，回写一次清理
                if ((int)wifiCount < total) {
                    Serial.printf("[WiFi] dedup: %d → %d, rewriting\n", total, wifiCount);
                    saveWifiNetworks();
                }
            }
        }
    }
    // 迁移：旧版本只有 config.json 没有 wifi_networks.json，自动迁移一次
    if (wifiCount == 0 && cfg.wifi_ssid[0] != '\0') {
        Serial.println(F("[WiFi] migrating wifi_ssid from config.json"));
        lruAddOrUpdate(cfg.wifi_ssid, cfg.wifi_pass);
        saveWifiNetworks();
    }
    Serial.printf("[WiFi] %d network(s) in history\n", wifiCount);
    return wifiCount > 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// writeFlag：在 LittleFS 写入空标志文件（用于跨重启传递模式信号）
// ─────────────────────────────────────────────────────────────────────────────
void writeFlag(const char* path) {
    File f = LittleFS.open(path, "w");
    if (f) f.close();
}

// ─────────────────────────────────────────────────────────────────────────────
// printWifiHistory：打印 WiFi 历史列表（两处 show 共用）
// ─────────────────────────────────────────────────────────────────────────────
static void printWifiHistory() {
    if (wifiCount == 0) {
        Serial.println(F("  (empty)"));
    } else {
        for (uint8_t i = 0; i < wifiCount; i++)
            Serial.printf("  [%d] %s\n", i, lruGet(i)->ssid);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// enterConfigMode：串口配置 CLI，保存后重启，永不正常返回
// ─────────────────────────────────────────────────────────────────────────────
static void printConfigHelp() {
    Serial.println(F("\n====== CONFIG MODE ======"));
    Serial.println(F("Firmware: " FW_VERSION));
    Serial.println(F("Commands:"));
    Serial.println(F("  set wifi_ssid   <value>  (配合 set wifi_pass 后执行 save 添加到历史列表)"));
    Serial.println(F("  set wifi_pass   <value>"));
    Serial.println(F("  wifi list        - 列出历史 WiFi 网络"));
    Serial.println(F("  wifi del <n>     - 删除第 n 条历史网络（从 0 开始）"));
    Serial.println(F("  set mqtt_server <value>"));
    Serial.println(F("  set mqtt_port   <value>  (default 8883)"));
    Serial.println(F("  set mqtt_user   <value>"));
    Serial.println(F("  set mqtt_pass   <value>"));
    Serial.println(F("  set wol_mac        <value>  (12 hex chars, no separators)"));
    Serial.println(F("  set wifi_tx_power  <2-20|0>      (dBm; 0=platform default; ESP32-C3 推荐 15)"));
    Serial.println(F("  id     - show device mqtt_id and BLE PSK"));
    Serial.println(F("  show   - display staged config"));
    Serial.println(F("  save   - write to flash and reboot"));
    Serial.println(F("  ble    - write /bleprov flag and reboot into BLE provisioning"));
    Serial.println(F("  reset  - erase config and reboot"));
    Serial.println(F("  reboot - reboot immediately"));
    Serial.println(F("  help   - show this help"));
    Serial.println(F("========================="));
}

void enterConfigMode() {
    DevConfig staged = cfg;                         // 以当前配置为起点（可能全空）
    if (staged.mqtt_port == 0) staged.mqtt_port = 8883;
    bool explicitSsid = false;                      // 用户是否显式执行过 set wifi_ssid

    printConfigHelp();

    String line;
    unsigned long lastBlink = 0;
    bool ledOn = false;

    while (true) {
        // 此循环永不返回，yield() 让出 CPU 给后台任务（WiFi/系统），避免看门狗复位。
        yield();

        // LED 100ms 快闪提示配置模式
        if (millis() - lastBlink >= 100) {
            ledOn = !ledOn;
            digitalWrite(LED_PIN, ledOn ? LOW : HIGH);
            lastBlink = millis();
        }

        while (Serial.available()) {
            char c = (char)Serial.read();
            if (c == '\r') continue;
            if (c != '\n') { line += c; continue; }

            // ── 收到换行，处理一行 ──
            line.trim();
            if (line.length() == 0) { line = ""; continue; }

            if (line == "help") {
                printConfigHelp();

            } else if (line == "show") {
                Serial.println(F("------ staged config ------"));
                Serial.println(F("[WiFi - pending]"));
                Serial.printf("  ssid      : %s\n", strlen(staged.wifi_ssid) ? staged.wifi_ssid : "(not set)");
                Serial.printf("  pass      : %s\n", strlen(staged.wifi_pass) ? "(set)"          : "(not set)");
                Serial.println(F("[WiFi history]"));
                printWifiHistory();
                Serial.println(F("[MQTT]"));
                Serial.printf("  server    : %s:%u\n", staged.mqtt_server, staged.mqtt_port);
                Serial.printf("  user      : %s\n",    staged.mqtt_user);
                Serial.printf("  pass      : %s\n",    strlen(staged.mqtt_pass) ? "(set)" : "(not set)");
                Serial.println(F("[Device]"));
                Serial.printf("  mqtt_id   : %s\n",  cfg.mqtt_id);
                Serial.printf("  wol_mac   : %s\n",  staged.wol_mac);
                Serial.printf("  tx_power  : %ddBm%s\n", staged.wifi_tx_power, staged.wifi_tx_power == 0 ? " (default)" : "");
                Serial.println(F("---------------------------"));

            } else if (line == "id") {
                printDeviceId();

            } else if (line == "wifi list") {
                if (wifiCount == 0) {
                    Serial.println(F("[WiFi] history empty"));
                } else {
                    Serial.printf("[WiFi] %d network(s):\n", wifiCount);
                    for (uint8_t i = 0; i < wifiCount; i++)
                        Serial.printf("  [%d] %s\n", i, lruGet(i)->ssid);
                }

            } else if (line.startsWith("wifi del ")) {
                int idx = line.substring(9).toInt();
                if (idx < 0 || (uint8_t)idx >= wifiCount) {
                    Serial.printf("[WiFi] invalid index %d (0-%d)\n", idx, wifiCount - 1);
                } else {
                    Serial.printf("[WiFi] removing [%d] %s\n", idx, lruGet(idx)->ssid);
                    lruRemoveAt(idx);
                    saveWifiNetworks();
                }

            } else if (line == "save") {
                bool ok = true;
                if (wifiCount == 0 && !explicitSsid) { Serial.println(F("[CFG] error: wifi_ssid required (or use 'wifi list' to check history)")); ok = false; }
                if (staged.mqtt_server[0] == '\0') { Serial.println(F("[CFG] error: mqtt_server required")); ok = false; }
                // wol_mac 可选：留空 = 仅响应指令里随包带 mac 的 wol；非空必须 12 hex
                if (staged.wol_mac[0] != '\0') {
                    bool hexOk = (strlen(staged.wol_mac) == 12);
                    for (int i = 0; hexOk && i < 12; i++) hexOk = isxdigit((unsigned char)staged.wol_mac[i]);
                    if (!hexOk) { Serial.println(F("[CFG] error: wol_mac must be empty or 12 hex chars")); ok = false; }
                }
                if (ok) {
                    // 仅当用户显式执行了 set wifi_ssid 时才写入历史列表
                    if (explicitSsid) {
                        lruAddOrUpdate(staged.wifi_ssid, staged.wifi_pass);
                        saveWifiNetworks();
                        Serial.printf("[CFG] added '%s' to wifi history\n", staged.wifi_ssid);
                    }
                    // cfg.wifi_ssid 始终指向列表第一条（最近使用的网络）
                    const WifiNode* head = lruGet(0);
                    if (head) {
                        strlcpy(staged.wifi_ssid, head->ssid, sizeof(staged.wifi_ssid));
                        strlcpy(staged.wifi_pass, head->pass, sizeof(staged.wifi_pass));
                    }
                    cfg = staged;
                    cfg.wifi_ever_ok = false;  // 重新配置后视为首次连接，超时进配置模式而非重启
                    if (saveConfig()) {
                        Serial.println(F("[CFG] saved, rebooting..."));
                        digitalWrite(LED_PIN, HIGH);
                        delay(300);
                        ESP.restart();
                    } else {
                        Serial.println(F("[CFG] ERROR: write failed, check LittleFS"));
                    }
                }

            } else if (line == "ble") {
                writeFlag("/bleprov");
                Serial.println(F("[CFG] /bleprov flag written, rebooting into BLE mode..."));
                digitalWrite(LED_PIN, HIGH);
                delay(300);
                ESP.restart();

            } else if (line == "reset") {
                LittleFS.remove("/config.json");
                LittleFS.remove("/wifi_networks.json");
                lruInit();
                Serial.println(F("[CFG] config + wifi history erased, rebooting..."));
                digitalWrite(LED_PIN, HIGH);
                delay(300);
                ESP.restart();

            } else if (line == "reboot") {
                Serial.println(F("[CFG] rebooting..."));
                digitalWrite(LED_PIN, HIGH);
                delay(300);
                ESP.restart();

            } else if (line.startsWith("set ")) {
                String rest = line.substring(4);
                rest.trim();
                int sp = rest.indexOf(' ');
                if (sp < 0) {
                    Serial.println(F("[CFG] usage: set <key> <value>"));
                } else {
                    String key = rest.substring(0, sp);
                    String val = rest.substring(sp + 1);
                    val.trim();

                    bool valid = true;
                    if      (key == "wifi_ssid")   { strlcpy(staged.wifi_ssid, val.c_str(), sizeof(staged.wifi_ssid)); explicitSsid = true; }
                    else if (key == "wifi_pass")   strlcpy(staged.wifi_pass,   val.c_str(), sizeof(staged.wifi_pass));
                    else if (key == "mqtt_server") strlcpy(staged.mqtt_server, val.c_str(), sizeof(staged.mqtt_server));
                    else if (key == "mqtt_user")   strlcpy(staged.mqtt_user,   val.c_str(), sizeof(staged.mqtt_user));
                    else if (key == "mqtt_pass")   strlcpy(staged.mqtt_pass,   val.c_str(), sizeof(staged.mqtt_pass));
                    else if (key == "mqtt_port") {
                        int p = val.toInt();
                        if (p > 0 && p <= 65535) staged.mqtt_port = (uint16_t)p;
                        else { Serial.println(F("[CFG] error: port must be 1-65535")); valid = false; }
                    } else if (key == "wol_mac") {
                        // 传 '-' 清除已存 MAC；其余必须是 12 位 hex
                        if (val == "-") {
                            staged.wol_mac[0] = '\0';
                        } else {
                            bool hexOk = (val.length() == 12);
                            for (int i = 0; hexOk && i < 12; i++) hexOk = isxdigit((unsigned char)val[i]);
                            if (!hexOk) { Serial.println(F("[CFG] error: wol_mac must be 12 hex chars (or '-' to clear)")); valid = false; }
                            else strlcpy(staged.wol_mac, val.c_str(), sizeof(staged.wol_mac));
                        }
                    } else if (key == "wifi_tx_power") {
                        int p = val.toInt();
                        if (p != 0 && (p < 2 || p > 20)) {
                            Serial.println(F("[CFG] error: wifi_tx_power must be 2-20 (dBm) or 0 for default"));
                            valid = false;
                        } else {
                            staged.wifi_tx_power = (int8_t)p;
                        }
                    } else {
                        Serial.printf("[CFG] unknown key: %s\n", key.c_str());
                        valid = false;
                    }

                    if (valid) {
                        Serial.printf("[CFG] %s = %s\n", key.c_str(), val.c_str());
                    }
                }

            } else {
                Serial.printf("[CFG] unknown command: %s (type 'help')\n", line.c_str());
            }

            line = "";
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// checkReconfigFlag：检测 /reconfig 标志，有则进入串口配置 CLI
// 由串口 "config" 命令或其他触发源在重启前写入
// ─────────────────────────────────────────────────────────────────────────────
void checkReconfigFlag() {
    if (!LittleFS.exists("/reconfig")) return;
    LittleFS.remove("/reconfig");
    Serial.println(F("[CFG] /reconfig flag → serial CLI"));
    enterConfigMode();  // 永不返回
}

// ─────────────────────────────────────────────────────────────────────────────
// applyAndSaveConfig：校验并落盘一组配置字段，返回 ""=成功，否则为错误描述。
//   withMqtt=false 时只写 WiFi/MAC,不触碰 MQTT 字段(保留现有值)。
// ─────────────────────────────────────────────────────────────────────────────
String applyAndSaveConfig(const String& ssidIn, const String& wpass, const String& wolmacIn,
                          bool withMqtt, const String& mqttsvrIn, int mqttport,
                          const String& mqttuser, const String& mqttpass) {
    String ssid   = ssidIn;   ssid.trim();
    String wolmac = wolmacIn; wolmac.trim();
    String mqttsvr = mqttsvrIn; mqttsvr.trim();

    if (ssid.length() == 0)   return "WiFi SSID 不能为空。";
    // wol_mac 可选：留空表示设备不存默认 MAC，仅响应指令里随包带的 mac
    if (wolmac.length() > 0) {
        if (wolmac.length() != 12) return "WoL MAC 地址留空或必须是 12 位十六进制字符（无分隔符）。";
        for (int i = 0; i < 12; i++)
            if (!isxdigit((unsigned char)wolmac[i])) return "WoL MAC 地址包含非十六进制字符。";
    }
    if (withMqtt && mqttsvr.length() == 0) return "MQTT 服务器地址不能为空。";

    // 写入 cfg 并更新 WiFi 历史列表
    lruAddOrUpdate(ssid.c_str(), wpass.c_str());
    saveWifiNetworks();
    strlcpy(cfg.wifi_ssid, lruGet(0)->ssid, sizeof(cfg.wifi_ssid));
    strlcpy(cfg.wifi_pass, lruGet(0)->pass, sizeof(cfg.wifi_pass));
    strlcpy(cfg.wol_mac,   wolmac.c_str(), sizeof(cfg.wol_mac));
    cfg.wifi_ever_ok = false;

    if (withMqtt) {
        strlcpy(cfg.mqtt_server, mqttsvr.c_str(), sizeof(cfg.mqtt_server));
        cfg.mqtt_port = (mqttport > 0 && mqttport <= 65535) ? (uint16_t)mqttport : 8883;
        strlcpy(cfg.mqtt_user, mqttuser.c_str(), sizeof(cfg.mqtt_user));
        strlcpy(cfg.mqtt_pass, mqttpass.c_str(), sizeof(cfg.mqtt_pass));
    }

    if (!saveConfig()) return "配置写入 LittleFS 失败。";
    return "";
}

// ═════════════════════════════════════════════════════════════════════════════
// BLE 配网模式（阶段 2：X25519 + AES-256-GCM 加密 GATT + 分片协议）
//   GATT 5 特征：command(写) status(读+通知) scan(读+通知) config(写) handshake(写+通知)
//   分片协议：每片头 4 字节 [总长 LE:2][序号:1][标志:1]，标志 bit0=末片
//   安全：客户端写 32B X25519 公钥 → 设备 ECDH 算共享密钥 →
//         session_key = HMAC-SHA256(PSK, shared‖"wol-ble-v1") → 回传设备公钥
//         config 载荷 = iv(12)‖tag(16)‖密文，AES-256-GCM；未握手拒收
//   PSK：首次开机随机生成 16B 存 /ble_psk.bin（工厂重置不删），串口打印 hex
//   详见 docs/BLE_REDESIGN.md
// ═════════════════════════════════════════════════════════════════════════════
#define BLE_SVC_UUID    "e0c1a700-2b3a-4f5c-9d7e-1a2b3c4d5e6f"
#define BLE_CMD_UUID    "e0c1a701-2b3a-4f5c-9d7e-1a2b3c4d5e6f"
#define BLE_STATUS_UUID "e0c1a702-2b3a-4f5c-9d7e-1a2b3c4d5e6f"
#define BLE_SCAN_UUID   "e0c1a703-2b3a-4f5c-9d7e-1a2b3c4d5e6f"
#define BLE_CFG_UUID    "e0c1a704-2b3a-4f5c-9d7e-1a2b3c4d5e6f"
#define BLE_HS_UUID     "e0c1a705-2b3a-4f5c-9d7e-1a2b3c4d5e6f"

#define BLE_FRAG_HEADER   4       // 分片头字节数
#define BLE_FRAG_CHUNK    180     // 分片 payload 上限（留足 MTU 余量）
#define BLE_CFG_BUF_SIZE  1024    // 配置载荷重组缓冲上限
#define BLE_PROV_TIMEOUT  (20UL * 60 * 1000) // 配网模式无 BLE 连接活动超时（ms，仅对已配置设备生效）
#define BLE_GCM_IV_LEN    12      // AES-GCM 随机 IV 长度
#define BLE_GCM_TAG_LEN   16      // AES-GCM 认证标签长度

// ── BLE 回调与主循环共享状态（volatile：回调在 NimBLE 任务，主循环在另一任务） ──
static volatile bool     bleClientConnected = false;
static volatile bool     bleScanRequested   = false;
static volatile bool     bleConfigReady     = false;
static volatile bool     bleRebootRequested = false;
static volatile bool     bleHandshakeReq    = false;   // 收到客户端公钥待处理
static uint8_t           bleClientPub[32];             // 客户端 X25519 公钥
static uint8_t           bleCfgBuf[BLE_CFG_BUF_SIZE];
static volatile uint16_t bleCfgLen   = 0;   // 已累积字节数
static volatile uint16_t bleCfgTotal = 0;   // 分片头声明的总字节数
static NimBLECharacteristic* bleStatusChar = nullptr;
static NimBLECharacteristic* bleScanChar   = nullptr;
static NimBLECharacteristic* bleHsChar     = nullptr;

// ── 加密状态（blePsk 声明见前「设备身份」一节） ───────────────────────────────
static uint8_t           bleSessionKey[32];     // 握手派生的 AES-256-GCM 会话密钥
static volatile bool     bleSessionReady = false;   // 握手是否完成

// mbedtls RNG 回调：转发到 ESP32 硬件随机数
static int bleRng(void*, unsigned char* buf, size_t len) {
    esp_fill_random(buf, len);
    return 0;
}

// 加载 /ble_psk.bin；不存在则随机生成并持久化。PSK 是设备身份，工厂重置不删除。
static void bleLoadOrCreatePsk() {
    File f = LittleFS.open(BLE_PSK_FILE, "r");
    if (f && f.size() == BLE_PSK_LEN) {
        f.read(blePsk, BLE_PSK_LEN);
        f.close();
        return;
    }
    if (f) f.close();
    esp_fill_random(blePsk, BLE_PSK_LEN);
    File w = LittleFS.open(BLE_PSK_FILE, "w");
    if (w) { w.write(blePsk, BLE_PSK_LEN); w.close(); }
    Serial.println(F("[BLE] generated new per-device PSK"));
}

// session_key = HMAC-SHA256(key=PSK, msg = shared ‖ "wol-ble-v1")
static void bleDeriveSessionKey(const uint8_t* shared, size_t sharedLen) {
    static const char label[] = "wol-ble-v1";
    const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, md, 1);   // 1 = 启用 HMAC
    mbedtls_md_hmac_starts(&ctx, blePsk, BLE_PSK_LEN);
    mbedtls_md_hmac_update(&ctx, shared, sharedLen);
    mbedtls_md_hmac_update(&ctx, (const uint8_t*)label, sizeof(label) - 1);
    mbedtls_md_hmac_finish(&ctx, bleSessionKey);
    mbedtls_md_free(&ctx);
}

// AES-256-GCM 解密 config 载荷 iv(12)‖tag(16)‖密文 → 明文。
// 返回明文长度；解密或验签失败返回 -1。
static int bleDecryptConfig(const uint8_t* in, size_t inLen, uint8_t* out, size_t outCap) {
    if (inLen < BLE_GCM_IV_LEN + BLE_GCM_TAG_LEN) return -1;
    size_t ctLen = inLen - BLE_GCM_IV_LEN - BLE_GCM_TAG_LEN;
    if (ctLen > outCap) return -1;
    const uint8_t* iv  = in;
    const uint8_t* tag = in + BLE_GCM_IV_LEN;
    const uint8_t* ct  = in + BLE_GCM_IV_LEN + BLE_GCM_TAG_LEN;
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    int r = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, bleSessionKey, 256);
    if (r == 0)
        r = mbedtls_gcm_auth_decrypt(&gcm, ctLen, iv, BLE_GCM_IV_LEN,
                                     nullptr, 0, tag, BLE_GCM_TAG_LEN, ct, out);
    mbedtls_gcm_free(&gcm);
    return (r == 0) ? (int)ctLen : -1;
}

// status 特征写入文本并通知（仅主循环调用）
static void bleSetStatus(const char* s) {
    if (!bleStatusChar) return;
    bleStatusChar->setValue((const uint8_t*)s, strlen(s));
    bleStatusChar->notify();
    Serial.printf("[BLE] status: %s\n", s);
}

// 按分片协议将一段数据经指定特征 notify 下发
static void bleNotifyFragmented(NimBLECharacteristic* chr, const uint8_t* data, uint16_t len) {
    uint8_t seq = 0;
    uint16_t off = 0;
    do {
        uint16_t plen = (len - off > BLE_FRAG_CHUNK) ? BLE_FRAG_CHUNK : (len - off);
        uint8_t pkt[BLE_FRAG_HEADER + BLE_FRAG_CHUNK];
        pkt[0] = (uint8_t)(len & 0xFF);
        pkt[1] = (uint8_t)(len >> 8);
        pkt[2] = seq++;
        pkt[3] = (off + plen >= len) ? 0x01 : 0x00;   // bit0 = 末片
        memcpy(pkt + BLE_FRAG_HEADER, data + off, plen);
        chr->setValue(pkt, BLE_FRAG_HEADER + plen);
        chr->notify();
        delay(30);   // 给客户端留接收间隙
        off += plen;
    } while (off < len);
}

// command 特征：接收 "scan" / "reboot" 文本指令
class BleCmdCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* chr, NimBLEConnInfo&) override {
        NimBLEAttValue v = chr->getValue();
        const char* d = (const char*)v.data();
        size_t n = v.length();
        if      (n == 4 && memcmp(d, "scan",   4) == 0) bleScanRequested   = true;
        else if (n == 6 && memcmp(d, "reboot", 6) == 0) bleRebootRequested = true;
    }
};

// config 特征：接收分片配置 JSON，按头部重组到 bleCfgBuf
class BleCfgCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* chr, NimBLEConnInfo&) override {
        NimBLEAttValue v = chr->getValue();
        const uint8_t* d = v.data();
        size_t n = v.length();
        if (n < BLE_FRAG_HEADER) return;
        uint16_t total = (uint16_t)d[0] | ((uint16_t)d[1] << 8);
        uint8_t  seq   = d[2];
        uint8_t  flags = d[3];
        size_t   plen  = n - BLE_FRAG_HEADER;
        if (seq == 0) { bleCfgLen = 0; bleCfgTotal = total; }   // 首片：复位缓冲
        if (bleCfgLen + plen <= sizeof(bleCfgBuf)) {
            memcpy(bleCfgBuf + bleCfgLen, d + BLE_FRAG_HEADER, plen);
            bleCfgLen += plen;
        }
        if (flags & 0x01) bleConfigReady = true;   // 末片：通知主循环处理
    }
};

// handshake 特征：接收 32 字节客户端 X25519 公钥
class BleHsCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* chr, NimBLEConnInfo&) override {
        NimBLEAttValue v = chr->getValue();
        if (v.length() != 32) return;
        memcpy(bleClientPub, v.data(), 32);
        bleHandshakeReq = true;
    }
};

// 服务端连接回调：维护连接状态，断开后恢复广播
class BleServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer*, NimBLEConnInfo&) override {
        bleClientConnected = true;
        Serial.println(F("[BLE] client connected"));
    }
    void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) override {
        bleClientConnected = false;
        bleSessionReady    = false;   // 会话密钥随连接失效，新客户端须重新握手
        Serial.println(F("[BLE] client disconnected, re-advertising"));
        NimBLEDevice::startAdvertising();
    }
};

// WiFi 扫描并经 scan 特征分片下发结果 JSON
static void bleDoScan() {
    int n = WiFi.scanNetworks();
    StaticJsonDocument<1024> doc;
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < n && i < 20; i++) {
        JsonObject o = arr.add<JsonObject>();
        o["ssid"] = WiFi.SSID(i);
        o["rssi"] = WiFi.RSSI(i);
    }
    WiFi.scanDelete();
    String json;
    serializeJson(doc, json);
    Serial.printf("[BLE] scan: %d network(s), %u bytes\n", n, json.length());
    bleNotifyFragmented(bleScanChar, (const uint8_t*)json.c_str(), json.length());
}

// 处理握手：用收到的客户端公钥做 X25519 ECDH，派生会话密钥，回传设备公钥
//
// 注意：ESP-IDF 的 mbedtls Curve25519 (Everest variant) 实现使用 TLS 线缆格式
// (`[长度前缀 0x20][32 字节公钥]`)，因此本地缓冲必须 ≥ 33 字节；BLE 线缆上
// 客户端发送/接收的仍是 raw 32 字节，需要在固件这一侧手动加/去长度前缀。
static void bleDoHandshake() {
    uint8_t devPubWire[33];     // [0]=0x20, [1..32]=公钥;给 mbedtls 用
    uint8_t cliPubWire[33];     // [0]=0x20, [1..32]=客户端公钥;给 mbedtls 用
    uint8_t shared[32];
    size_t  olen = 0;
    int     rc   = 0;
    int     stage = 0;
    mbedtls_ecdh_context ecdh;
    mbedtls_ecdh_init(&ecdh);

    // 打印收到的客户端公钥(诊断用)
    Serial.print(F("[BLE] client pubkey: "));
    for (int i = 0; i < 32; i++) Serial.printf("%02x", bleClientPub[i]);
    Serial.println();

    // 构造 TLS 格式的客户端公钥:1 字节长度 + 32 字节 key
    cliPubWire[0] = 32;
    memcpy(cliPubWire + 1, bleClientPub, 32);

    do {
        stage = 1;
        rc = mbedtls_ecdh_setup(&ecdh, MBEDTLS_ECP_DP_CURVE25519);
        if (rc != 0) break;

        stage = 2;
        rc = mbedtls_ecdh_make_public(&ecdh, &olen, devPubWire, sizeof(devPubWire),
                                      bleRng, nullptr);
        if (rc != 0) break;
        if (olen != 33) { rc = -0xAAAA; break; }   // 期望 1 字节前缀 + 32 字节

        stage = 3;
        rc = mbedtls_ecdh_read_public(&ecdh, cliPubWire, sizeof(cliPubWire));
        if (rc != 0) break;

        stage = 4;
        rc = mbedtls_ecdh_calc_secret(&ecdh, &olen, shared, sizeof(shared),
                                      bleRng, nullptr);
        if (rc != 0) break;
        if (olen != 32) { rc = -0xBBBB; break; }

        stage = 0;   // 全部成功
    } while (0);
    mbedtls_ecdh_free(&ecdh);

    if (stage != 0) {
        Serial.printf("[BLE] handshake (X25519) failed at stage %d, rc=-0x%04x\n",
                      stage, (unsigned)(-rc) & 0xFFFF);
        bleSetStatus("error:密钥交换失败。");
        return;
    }

    // 把 raw 32 字节设备公钥指针保存到 devPub(去掉 TLS 长度前缀),后续 notify 用
    const uint8_t* devPub = devPubWire + 1;
    bleDeriveSessionKey(shared, 32);
    bleSessionReady = true;
    bleHsChar->setValue(devPub, 32);
    bleHsChar->notify();
    Serial.println(F("[BLE] handshake done, session key established"));
    bleSetStatus("handshake_ok");
}

// 解密并解析配置载荷，校验落盘；成功则重启（不返回），失败经 status 回报
static void bleProcessConfig() {
    bleConfigReady = false;
    uint16_t encLen = bleCfgLen;
    bleCfgLen = 0;

    if (!bleSessionReady) {
        Serial.println(F("[BLE] config rejected: handshake not done"));
        bleSetStatus("error:未完成密钥交换，拒收配置。");
        return;
    }

    // 解密 iv(12)‖tag(16)‖密文 → 明文（验签失败即 PSK 不符或数据被篡改）
    static uint8_t plain[BLE_CFG_BUF_SIZE];
    int plainLen = bleDecryptConfig(bleCfgBuf, encLen, plain, sizeof(plain));
    if (plainLen < 0) {
        Serial.println(F("[BLE] config decrypt/auth failed"));
        bleSetStatus("error:解密验签失败（PSK 不匹配或数据损坏）。");
        return;
    }

    StaticJsonDocument<768> doc;
    DeserializationError e = deserializeJson(doc, plain, plainLen);
    if (e) {
        Serial.printf("[BLE] config JSON parse error: %s\n", e.c_str());
        bleSetStatus("error:配置 JSON 解析失败。");
        return;
    }
    String mqttsvr = doc["mqttsvr"] | "";
    bool withMqtt  = mqttsvr.length() > 0;   // 携带 MQTT 字段时整套写入
    String err = applyAndSaveConfig(
        doc["ssid"] | "", doc["wpass"] | "", doc["wolmac"] | "",
        withMqtt, mqttsvr, doc["mqttport"] | 8883,
        doc["mqttuser"] | "", doc["mqttpass"] | "");
    if (err.length() > 0) {
        bleSetStatus((String("error:") + err).c_str());
        return;
    }
    bleSetStatus("ok:rebooting");
    digitalWrite(LED_PIN, LOW);   // LED 常亮提示配置成功
    delay(1000);                  // 给 notify 发送窗口
    ESP.restart();
}

// ─────────────────────────────────────────────────────────────────────────────
// enterBLEProvMode：启动 NimBLE GATT 配网服务，永不返回
// ─────────────────────────────────────────────────────────────────────────────
void enterBLEProvMode() {
    char devName[20];
    snprintf(devName, sizeof(devName), "WoL-%04X", (uint16_t)(ESP.getEfuseMac() & 0xFFFF));
    Serial.printf("\n[BLE] provisioning mode — device name: %s\n", devName);

    // mqtt_id 与 PSK 已在 setup 中派生/加载，此处打印供测试与贴二维码使用
    Serial.printf("[BLE] device mqtt_id: %s\n", cfg.mqtt_id);
    Serial.print(F("[BLE] device PSK (hex): "));
    for (int i = 0; i < BLE_PSK_LEN; i++) Serial.printf("%02x", blePsk[i]);
    Serial.println();
    bleSessionReady = false;

    // WiFi 置 STA 模式供扫描用；BLE 与 WiFi 在 ESP32-C3 上共存
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);

    NimBLEDevice::init(devName);
    NimBLEDevice::setMTU(247);

    NimBLEServer* server = NimBLEDevice::createServer();
    server->setCallbacks(new BleServerCallbacks());

    NimBLEService* svc = server->createService(BLE_SVC_UUID);
    NimBLECharacteristic* cmdChr =
        svc->createCharacteristic(BLE_CMD_UUID, NIMBLE_PROPERTY::WRITE);
    cmdChr->setCallbacks(new BleCmdCallbacks());

    bleStatusChar =
        svc->createCharacteristic(BLE_STATUS_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    bleStatusChar->setValue("idle");

    bleScanChar =
        svc->createCharacteristic(BLE_SCAN_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);

    NimBLECharacteristic* cfgChr =
        svc->createCharacteristic(BLE_CFG_UUID, NIMBLE_PROPERTY::WRITE);
    cfgChr->setCallbacks(new BleCfgCallbacks());

    bleHsChar =
        svc->createCharacteristic(BLE_HS_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY);
    bleHsChar->setCallbacks(new BleHsCallbacks());

    svc->start();

    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(BLE_SVC_UUID);
    adv->setName(devName);
    adv->enableScanResponse(true);
    adv->start();
    Serial.println(F("[BLE] advertising started, waiting for client..."));
    Serial.println(F("[BLE] serial recovery: type 'config' for serial CLI"));

    // 设备是否已有有效配置：决定超时行为（无配置时重启只会再进本模式，故常驻）
    bool hasConfig = LittleFS.exists("/config.json");
    unsigned long lastActivityMs = millis();  // 客户端连接期间持续刷新，断开后开始倒计时
    unsigned long lastBlink = 0;
    bool ledOn = false;

    while (true) {
        yield();

        // 串口恢复通道：BLE 配网模式下仍可输入 config 进串口 CLI（隐藏恢复通道）。
        // 无有效配置时设备直接进本模式，串口是离开此模式的唯一兜底入口。
        while (Serial.available()) {
            char c = (char)Serial.read();
            if (c == '\r') continue;
            if (c == '\n') {
                serialLineBuf.trim();
                if (serialLineBuf == "config") {
                    Serial.println(F("[BLE] serial → /reconfig, rebooting into serial CLI..."));
                    writeFlag("/reconfig");
                    delay(100);
                    ESP.restart();
                } else if (serialLineBuf == "reboot") {
                    Serial.println(F("[BLE] serial → rebooting..."));
                    delay(100);
                    ESP.restart();
                } else if (serialLineBuf == "id") {
                    printDeviceId();
                }
                serialLineBuf = "";
            } else if (serialLineBuf.length() < 128) {
                serialLineBuf += c;
            }
        }

        // LED 闪烁：未连接 500ms 慢闪，客户端已连接 150ms 快闪（连接反馈）
        if (millis() - lastBlink >= (bleClientConnected ? 150UL : 500UL)) {
            ledOn = !ledOn;
            digitalWrite(LED_PIN, ledOn ? LOW : HIGH);
            lastBlink = millis();
        }

        // 无 BLE 连接活动超时 → 重启回正常运行（防按键误触/连上又中途放弃使设备长期滞留）。
        // 连接期间持续刷新计时器，仅在客户端断开/未接入时累计；仅对已有配置的设备生效，
        // 无配置设备常驻配网（重启也只会再进本模式）。
        if (bleClientConnected) lastActivityMs = millis();
        if (hasConfig && millis() - lastActivityMs > BLE_PROV_TIMEOUT) {
            Serial.println(F("[BLE] provisioning idle timeout → restart"));
            delay(100);
            ESP.restart();
        }

        if (bleRebootRequested) {
            Serial.println(F("[BLE] reboot requested by client"));
            delay(200);
            ESP.restart();
        }

        if (bleHandshakeReq) {
            bleHandshakeReq = false;
            bleDoHandshake();
        }

        if (bleScanRequested) {
            bleScanRequested = false;
            bleSetStatus("scanning");
            bleDoScan();
            bleSetStatus("idle");
        }

        if (bleConfigReady) {
            Serial.printf("[BLE] config received: %u/%u bytes\n", bleCfgLen, bleCfgTotal);
            bleProcessConfig();   // 成功则重启不返回
        }

        delay(20);
    }
}

// checkBLEProvFlag：检测 /bleprov 标志，有则进入 BLE 配网模式
void checkBLEProvFlag() {
    if (!LittleFS.exists("/bleprov")) return;
    LittleFS.remove("/bleprov");
    Serial.println(F("[CFG] /bleprov flag → BLE provisioning mode"));
    enterBLEProvMode();  // 永不返回
}

// ─────────────────────────────────────────────────────────────────────────────
// checkRuntimeTriggers：可在任意循环中调用，非阻塞
//   串口输入 "config"  → 写 /reconfig，重启进入串口 CLI（隐藏恢复通道）
//   串口输入 "ble"     → 写 /bleprov，重启进入 BLE 配网模式
//   串口输入 "reboot"  → 直接重启
//   串口输入 "show"    → 打印当前运行状态（不重启）
//   串口输入 "id"      → 打印设备 mqtt_id 与 BLE PSK（不重启）
//   按键按住 3s 松手   → 写 /bleprov，重启进入 BLE 配网（配置保留，LED 双闪提示）
//   按键按住 10s      → 工厂重置（清空配置）后进入 BLE 配网（LED 五闪确认）
// ─────────────────────────────────────────────────────────────────────────────
void checkRuntimeTriggers() {
    // ── 串口输入检测 ──
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\r') continue;
        if (c == '\n') {
            serialLineBuf.trim();
            if (serialLineBuf == "config") {
                Serial.println(F("[CFG] serial trigger → writing /reconfig, rebooting..."));
                writeFlag("/reconfig");
                delay(100);
                ESP.restart();
            } else if (serialLineBuf == "ble") {
                Serial.println(F("[CFG] serial trigger → writing /bleprov, rebooting into BLE mode..."));
                writeFlag("/bleprov");
                delay(100);
                ESP.restart();
            } else if (serialLineBuf == "reboot") {
                Serial.println(F("[CFG] serial trigger → rebooting..."));
                delay(100);
                ESP.restart();
            } else if (serialLineBuf == "show") {
                Serial.println(F("-------- status --------"));
                Serial.println(F("[System]"));
                Serial.printf("  firmware  : %s\n",   FW_VERSION);
                Serial.printf("  uptime    : %lus\n", millis() / 1000);
                Serial.printf("  heap      : %u B\n", ESP.getFreeHeap());
                Serial.println(F("[WiFi]"));
                Serial.printf("  ssid      : %s\n",   cfg.wifi_ssid);
                Serial.printf("  ip        : %s\n",   WiFi.localIP().toString().c_str());
                Serial.printf("  rssi      : %ddBm\n", WiFi.RSSI());
                Serial.println(F("[WiFi history]"));
                printWifiHistory();
                Serial.println(F("[MQTT]"));
                Serial.printf("  server    : %s:%u\n", cfg.mqtt_server, cfg.mqtt_port);
                Serial.printf("  id        : %s\n",    cfg.mqtt_id);
                Serial.printf("  reconnects: %u\n",    mqttReconnectCount);
                Serial.println(F("[Device]"));
                Serial.printf("  wol_mac   : %s\n",  cfg.wol_mac);
                Serial.printf("  interval  : %lus\n", statusIntervalMs / 1000);
                Serial.println(F("------------------------"));
            } else if (serialLineBuf == "id") {
                printDeviceId();
            } else if (serialLineBuf == "netscan") {
                if (nsStart(false)) Serial.println(F("[NS] scanning... (results to serial)"));
                else                Serial.println(F("[NS] busy or WiFi not connected"));
            }
            serialLineBuf = "";
        } else {
            if (serialLineBuf.length() < 128) serialLineBuf += c;
        }
    }

    // ── 按键检测 ──
    bool btnDown = (digitalRead(CFG_RESET_PIN) == LOW);
    unsigned long elapsed = btnPressing ? millis() - btnPressStart : 0;

    if (btnDown && !btnPressing) {
        // 刚按下
        btnPressing   = true;
        btnHit3s      = false;
        btnPressStart = millis();
        Serial.println(F("[CFG] button held — 3s: BLE 配网, 10s: 工厂重置 + BLE 配网"));

    } else if (btnDown && btnPressing) {
        if (elapsed >= 10000) {
            // 长按 10s：清空配置（工厂重置）后进 BLE 配网，LED 五闪确认
            Serial.println(F("[CFG] 10s → factory reset, rebooting into BLE provisioning..."));
            LittleFS.remove("/config.json");
            LittleFS.remove("/wifi_networks.json");
            writeFlag("/bleprov");
            for (int i = 0; i < 10; i++) {
                digitalWrite(LED_PIN, i % 2 == 0 ? LOW : HIGH);
                delay(100);
            }
            digitalWrite(LED_PIN, HIGH);
            ESP.restart();
        } else if (elapsed >= 3000 && !btnHit3s) {
            // 刚跨过 3s 阈值：LED 双闪提示可松手进 BLE 配网
            btnHit3s = true;
            Serial.println(F("[CFG] release for BLE provisioning, keep 10s for factory reset"));
            for (int i = 0; i < 4; i++) {
                digitalWrite(LED_PIN, i % 2 == 0 ? LOW : HIGH);
                delay(100);
            }
            digitalWrite(LED_PIN, HIGH);
        }

    } else if (!btnDown && btnPressing) {
        // 松手
        btnPressing = false;
        if (btnHit3s) {
            // 松手在 3-10s 之间：写 /bleprov 标志，重启进 BLE 配网模式
            Serial.println(F("[CFG] 3s release → /bleprov flag, rebooting into BLE provisioning..."));
            writeFlag("/bleprov");
            for (int i = 0; i < 4; i++) {
                digitalWrite(LED_PIN, i % 2 == 0 ? LOW : HIGH);
                delay(150);
            }
            digitalWrite(LED_PIN, HIGH);
            ESP.restart();
        } else {
            Serial.println(F("[CFG] button released, cancelled"));
        }
        btnHit3s = false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// pubEvent：发布标准事件（自动附加 uptime/heap）
// ─────────────────────────────────────────────────────────────────────────────
void pubEvent(const char* event) {
    char buf[128];
    snprintf(buf, sizeof(buf),
        "{\"event\":\"%s\",\"uptime\":%lu,\"heap\":%u}",
        event, millis() / 1000, ESP.getFreeHeap());
    mqtt.publish(TOPIC_PUB, buf);
    lastStatusMs = millis();
}

// pubJson：发布 JSON 对象（自动附加 uptime/heap/ip）
void pubJson(JsonDocument& doc, bool retain = false) {
    doc["uptime"] = millis() / 1000;
    doc["heap"]   = ESP.getFreeHeap();
    doc["ip"]     = WiFi.localIP().toString();

    char buf[768];
    serializeJson(doc, buf, sizeof(buf));
    mqtt.publish(TOPIC_PUB, buf, retain);
    lastStatusMs = millis();
}

// pubReboot：发布重启事件后立即重启
void pubReboot(const char* reason) {
    char buf[96];
    snprintf(buf, sizeof(buf),
        "{\"event\":\"reboot\",\"reason\":\"%s\",\"uptime\":%lu,\"heap\":%u}",
        reason, millis() / 1000, ESP.getFreeHeap());
    mqtt.publish(TOPIC_PUB, buf);
    delay(300);
    ESP.restart();
}

// ─────────────────────────────────────────────────────────────────────────────
// sendMagicPacket：发送 WoL 魔法包（UDP 广播）
// ─────────────────────────────────────────────────────────────────────────────
void sendMagicPacket(const char* mac_str) {
    uint8_t mac[6];
    for (int i = 0; i < 6; i++) {
        char hex[3] = { mac_str[i * 2], mac_str[i * 2 + 1], '\0' };
        mac[i] = (uint8_t)strtol(hex, nullptr, 16);
    }

    uint8_t packet[102];
    memset(packet, 0xFF, 6);
    for (int i = 1; i <= 16; i++)
        memcpy(&packet[i * 6], mac, 6);

    uint32_t ip   = (uint32_t)WiFi.localIP();
    uint32_t mask = (uint32_t)WiFi.subnetMask();
    IPAddress bcast(ip | ~mask);

    udp.beginPacket(bcast, 9);
    udp.write(packet, sizeof(packet));
    udp.endPacket();

    Serial.printf("[WoL] magic packet → %s (mac: %s)\n", bcast.toString().c_str(), mac_str);
}

// ═════════════════════════════════════════════════════════════════════════════
// 局域网发现 net_scan（阶段 1）
//   非阻塞状态机：每轮 loop 推进一步，不阻塞 MQTT keepalive。
//   IDLE → ARP_SEND（发一窗 ARP 请求）→ ARP_HARVEST（等 ~200ms 收割 lwIP ARP 表）
//        → [循环直到扫完 /24] → NAME_QUERY（逐台 NBNS/mDNS）→ REPORT（分批上报）→ IDLE
//   lwIP ARP 表默认 10 槽，故每窗只发 8 个请求，收割后转存自有数组再发下一窗。
//   只扫自身所在 /24，结果上限 NETSCAN_MAX_HOSTS。详见 docs/NET_SCAN.md。
//   类型 NetScanState / ScanHost 定义在文件顶部（供 .ino 自动生成的原型可见）。
// ═════════════════════════════════════════════════════════════════════════════
static NetScanState  nsState     = NS_IDLE;
static ScanHost      nsHosts[NETSCAN_MAX_HOSTS];
static uint8_t       nsHostCount = 0;
static struct netif* nsNetif     = nullptr;
static uint32_t      nsSelfIp    = 0;     // 本机 IP（网络字节序）
static uint32_t      nsGwIp      = 0;     // 网关 IP（网络字节序）
static uint32_t      nsBase24    = 0;     // /24 网络地址（host 位清零）
static uint16_t      nsCurHost   = 0;     // 当前扫描到的 host 位（1..254）
static uint8_t       nsWinCount  = 0;     // 本窗已发请求数
static uint32_t      nsWinIps[NETSCAN_ARP_WINDOW];
static unsigned long nsTimer     = 0;
static uint8_t       nsNameIdx   = 0;     // 名字查询进度
static uint8_t       nsReportSeq = 0;     // 上报批次序号
static bool          nsViaMqtt   = false; // true=MQTT 上报，false=串口本地验证

// 定位 WiFi STA 对应的 lwIP netif（按本机 IP 匹配，兜底 netif_default）
static struct netif* nsFindStaNetif() {
    for (struct netif* nif = netif_list; nif != nullptr; nif = nif->next)
        if (netif_ip4_addr(nif)->addr == nsSelfIp) return nif;
    return netif_default;
}

// 解析 NBNS NBSTAT 应答，提取首个 workstation(后缀 0x00, 非组) 名字，去尾部空格
static bool nsParseNbns(const uint8_t* r, int n, char* out, size_t cap) {
    if (n < 12) return false;
    int p = 12;                                   // 跳过 DNS 头
    if ((r[p] & 0xC0) == 0xC0) p += 2;            // 压缩指针
    else { while (p < n && r[p] != 0) p += r[p] + 1; p += 1; }   // 逐 label 至 0
    p += 2 + 2 + 4;                               // type(2) class(2) ttl(4)
    if (p + 2 > n) return false;
    p += 2;                                        // rdlength(2)
    if (p >= n) return false;
    int numNames = r[p++];
    for (int i = 0; i < numNames && p + 18 <= n; i++) {
        const uint8_t* name = r + p;
        uint8_t  suffix = r[p + 15];
        uint16_t flags  = ((uint16_t)r[p + 16] << 8) | r[p + 17];
        bool     group  = flags & 0x8000;
        if (suffix == 0x00 && !group) {
            int end = 15;
            while (end > 0 && (name[end - 1] == ' ' || name[end - 1] == 0)) end--;
            int copy = (end < (int)cap - 1) ? end : (int)cap - 1;
            memcpy(out, name, copy);
            out[copy] = '\0';
            return out[0] != '\0';
        }
        p += 18;
    }
    return false;
}

// 解码 DNS 名字的首个 label（处理压缩指针），用于 mDNS PTR 应答取主机名
static bool nsDecodeFirstLabel(const uint8_t* r, int n, int p, char* out, size_t cap) {
    int outpos = 0, safety = 0;
    while (p >= 0 && p < n && safety++ < 32) {
        uint8_t len = r[p];
        if (len == 0) break;
        if ((len & 0xC0) == 0xC0) {                // 压缩指针 → 跳转
            if (p + 1 >= n) break;
            p = ((len & 0x3F) << 8) | r[p + 1];
            continue;
        }
        p++;
        for (int i = 0; i < len && p < n; i++, p++)
            if (outpos < (int)cap - 1) out[outpos++] = r[p];
        break;                                     // 只取首 label（主机名，跳过 .local）
    }
    out[outpos] = '\0';
    return outpos > 0;
}

// 跳过一个 DNS 名字字段，返回其后偏移
static int nsSkipName(const uint8_t* r, int n, int p) {
    while (p < n) {
        uint8_t len = r[p];
        if (len == 0)              return p + 1;
        if ((len & 0xC0) == 0xC0)  return p + 2;
        p += len + 1;
    }
    return p;
}

// 解析 mDNS 反查 PTR 应答，取主机名首 label
static bool nsParseMdns(const uint8_t* r, int n, char* out, size_t cap) {
    if (n < 12) return false;
    int qd = ((int)r[4] << 8) | r[5];
    int an = ((int)r[6] << 8) | r[7];
    if (an < 1) return false;
    int p = 12;
    for (int i = 0; i < qd; i++) { p = nsSkipName(r, n, p); p += 4; }  // 跳问题区
    p = nsSkipName(r, n, p);                       // 应答 name
    if (p + 10 > n) return false;
    int type = ((int)r[p] << 8) | r[p + 1]; p += 2;
    p += 2 + 4;                                    // class(2) ttl(4)
    int rdlen = ((int)r[p] << 8) | r[p + 1]; p += 2;
    if (type != 0x0C || p + rdlen > n) return false;   // 0x0C = PTR
    return nsDecodeFirstLabel(r, n, p, out, cap);
}

// 向目标 UDP 137 发 NBSTAT 通配查询（*），解析应答主机名（单包问答，超时 300ms）
static void nsNbnsQuery(ScanHost& h) {
    WiFiUDP u;
    if (!u.begin(0)) return;                       // 临时本地端口收应答
    uint8_t q[50];
    int p = 0;
    uint16_t txid = (uint16_t)(millis() & 0xFFFF);
    q[p++] = txid >> 8; q[p++] = txid & 0xFF;      // ID
    q[p++] = 0x00; q[p++] = 0x00;                  // flags
    q[p++] = 0x00; q[p++] = 0x01;                  // QDCOUNT=1
    q[p++] = 0x00; q[p++] = 0x00;                  // ANCOUNT
    q[p++] = 0x00; q[p++] = 0x00;                  // NSCOUNT
    q[p++] = 0x00; q[p++] = 0x00;                  // ARCOUNT
    q[p++] = 0x20;                                  // name 长度 32
    q[p++] = 'C'; q[p++] = 'K';                    // '*' 的 NetBIOS 一级编码
    for (int i = 0; i < 30; i++) q[p++] = 'A';     // 15 个 NUL 的编码
    q[p++] = 0x00;                                  // name 结束
    q[p++] = 0x00; q[p++] = 0x21;                  // type NBSTAT
    q[p++] = 0x00; q[p++] = 0x01;                  // class IN

    u.beginPacket(IPAddress(h.ip), 137);
    u.write(q, p);
    u.endPacket();

    unsigned long t = millis();
    while (millis() - t < NETSCAN_NAME_TIMEOUT_MS) {
        if (u.parsePacket() > 0) {
            uint8_t resp[256];
            int n = u.read(resp, sizeof(resp));
            if (n > 0 && nsParseNbns(resp, n, h.name, sizeof(h.name))) h.src = 1;
            break;
        }
        delay(10);
    }
    u.stop();
}

// 向目标 UDP 5353 发 <reversed-ip>.in-addr.arpa PTR 单播查询，解析主机名
static void nsMdnsQuery(ScanHost& h) {
    WiFiUDP u;
    if (!u.begin(0)) return;
    uint8_t q[64];
    int p = 0;
    q[p++] = 0x00; q[p++] = 0x00;                  // ID（mDNS 用 0）
    q[p++] = 0x00; q[p++] = 0x00;                  // flags
    q[p++] = 0x00; q[p++] = 0x01;                  // QDCOUNT=1
    q[p++] = 0x00; q[p++] = 0x00;
    q[p++] = 0x00; q[p++] = 0x00;
    q[p++] = 0x00; q[p++] = 0x00;
    // h.ip 为网络字节序：byte0=a..byte3=d；PTR 需逆序 d.c.b.a
    for (int i = 3; i >= 0; i--) {
        uint8_t v = (h.ip >> (i * 8)) & 0xFF;
        char num[4];
        int l = snprintf(num, sizeof(num), "%u", v);
        q[p++] = (uint8_t)l;
        memcpy(q + p, num, l); p += l;
    }
    static const char* suf[2] = { "in-addr", "arpa" };
    for (int s = 0; s < 2; s++) {
        int l = strlen(suf[s]);
        q[p++] = (uint8_t)l;
        memcpy(q + p, suf[s], l); p += l;
    }
    q[p++] = 0x00;                                  // root
    q[p++] = 0x00; q[p++] = 0x0C;                  // type PTR
    q[p++] = 0x00; q[p++] = 0x01;                  // class IN

    u.beginPacket(IPAddress(h.ip), 5353);
    u.write(q, p);
    u.endPacket();

    unsigned long t = millis();
    while (millis() - t < NETSCAN_NAME_TIMEOUT_MS) {
        if (u.parsePacket() > 0) {
            uint8_t resp[300];
            int n = u.read(resp, sizeof(resp));
            if (n > 0 && nsParseMdns(resp, n, h.name, sizeof(h.name))) h.src = 2;
            break;
        }
        delay(10);
    }
    u.stop();
}

// 启动一次扫描（busy 时返回 false）。viaMqtt=true 经 MQTT 上报，false 仅串口。
static bool nsStart(bool viaMqtt) {
    if (nsState != NS_IDLE) return false;
    if (WiFi.status() != WL_CONNECTED) return false;

    nsSelfIp  = (uint32_t)WiFi.localIP();
    nsGwIp    = (uint32_t)WiFi.gatewayIP();
    nsBase24  = nsSelfIp & 0x00FFFFFF;             // 清零 host 位（第 4 octet）
    nsNetif   = nsFindStaNetif();
    nsHostCount = 0;
    nsCurHost   = 1;
    nsNameIdx   = 0;
    nsReportSeq = 0;
    nsViaMqtt   = viaMqtt;
    nsState     = NS_ARP_SEND;

    char subnet[20];
    snprintf(subnet, sizeof(subnet), "%u.%u.%u.0/24",
             (unsigned)(nsBase24 & 0xFF), (unsigned)((nsBase24 >> 8) & 0xFF),
             (unsigned)((nsBase24 >> 16) & 0xFF));
    Serial.printf("[NS] scan start: %s (self=%s gw=%s)\n", subnet,
                  IPAddress(nsSelfIp).toString().c_str(),
                  IPAddress(nsGwIp).toString().c_str());

    if (nsViaMqtt) {
        StaticJsonDocument<160> doc;
        doc["event"]  = "net_scan_start";
        doc["subnet"] = subnet;
        doc["hosts"]  = 254;
        pubJson(doc);
    }
    return true;
}

// 发布（或串口打印）一批结果
static void nsReportBatch() {
    uint8_t start = nsReportSeq * NETSCAN_BATCH;
    uint8_t end   = start + NETSCAN_BATCH;
    if (end > nsHostCount) end = nsHostCount;
    bool last = (end >= nsHostCount);

    StaticJsonDocument<1024> doc;
    doc["event"] = "net_scan_result";
    doc["seq"]   = nsReportSeq;
    doc["last"]  = last;
    JsonArray arr = doc.createNestedArray("hosts");
    for (uint8_t i = start; i < end; i++) {
        ScanHost& h = nsHosts[i];
        JsonObject o = arr.createNestedObject();
        char ipStr[16];
        snprintf(ipStr, sizeof(ipStr), "%u.%u.%u.%u",
                 (unsigned)(h.ip & 0xFF), (unsigned)((h.ip >> 8) & 0xFF),
                 (unsigned)((h.ip >> 16) & 0xFF), (unsigned)((h.ip >> 24) & 0xFF));
        o["ip"] = ipStr;
        char macStr[13];
        snprintf(macStr, sizeof(macStr), "%02X%02X%02X%02X%02X%02X",
                 h.mac[0], h.mac[1], h.mac[2], h.mac[3], h.mac[4], h.mac[5]);
        o["mac"] = macStr;
        if (h.name[0]) {
            o["name"] = h.name;
            o["src"]  = (h.src == 1) ? "nbns" : "mdns";
        }
        if (h.mac[0] & 0x02) o["rand"] = true;     // 本地管理位 → 大概率随机 MAC
    }

    if (nsViaMqtt) {
        pubJson(doc);
    } else {
        char buf[768];
        serializeJson(doc, buf, sizeof(buf));
        Serial.printf("[NS] %s\n", buf);
    }
    nsReportSeq++;
    if (last) {
        nsState = NS_IDLE;
        Serial.printf("[NS] done: %u host(s)\n", nsHostCount);
    }
}

// net_scan 状态机推进：每轮 loop 调用一次，非阻塞（名字查询每轮处理一台）
static void nsTick() {
    switch (nsState) {
    case NS_IDLE:
        return;

    case NS_ARP_SEND: {
        nsWinCount = 0;
        LOCK_TCPIP_CORE();
        while (nsCurHost <= 254 && nsWinCount < NETSCAN_ARP_WINDOW) {
            uint32_t ip = nsBase24 | ((uint32_t)nsCurHost << 24);
            nsCurHost++;
            if (ip == nsSelfIp || ip == nsGwIp) continue;   // 排除自身、网关
            ip4_addr_t a; a.addr = ip;
            etharp_request(nsNetif, &a);
            nsWinIps[nsWinCount++] = ip;
        }
        UNLOCK_TCPIP_CORE();
        nsTimer = millis();
        nsState = NS_ARP_HARVEST;
        break;
    }

    case NS_ARP_HARVEST: {
        if (millis() - nsTimer < NETSCAN_HARVEST_MS) return;
        LOCK_TCPIP_CORE();
        for (uint8_t i = 0; i < nsWinCount && nsHostCount < NETSCAN_MAX_HOSTS; i++) {
            ip4_addr_t a; a.addr = nsWinIps[i];
            struct eth_addr*  eth = nullptr;
            const ip4_addr_t* ipr = nullptr;
            if (etharp_find_addr(nsNetif, &a, &eth, &ipr) >= 0 && eth) {
                ScanHost& h = nsHosts[nsHostCount++];
                h.ip = nsWinIps[i];
                memcpy(h.mac, eth->addr, 6);
                h.name[0] = '\0';
                h.src = 0;
            }
        }
        UNLOCK_TCPIP_CORE();
        if (nsCurHost <= 254 && nsHostCount < NETSCAN_MAX_HOSTS) {
            nsState = NS_ARP_SEND;                  // 继续下一窗
        } else {
            Serial.printf("[NS] ARP sweep done: %u host(s), resolving names...\n", nsHostCount);
            nsNameIdx = 0;
            nsState   = NS_NAME_QUERY;
        }
        break;
    }

    case NS_NAME_QUERY: {
        if (nsNameIdx >= nsHostCount) {
            nsReportSeq = 0;
            nsState     = NS_REPORT;
            return;
        }
        ScanHost& h = nsHosts[nsNameIdx];
        nsNbnsQuery(h);                             // 先 NBNS（Windows）
        if (h.name[0] == '\0') nsMdnsQuery(h);     // 无名再 mDNS 反查（mac/linux）
        nsNameIdx++;
        break;
    }

    case NS_REPORT:
        nsReportBatch();
        break;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// 唤醒验证 wake verify（wol verify:true，阶段 2）
//   发完魔法包后，独立轻量状态机每秒探测目标是否真的上线：复用 net_scan 的 lwIP
//   ARP 直读。按 MAC（而非只信 IP）判定——DHCP 可能换 IP，MAC 才是稳定标识：
//     - 每秒先扫 lwIP ARP 表找目标 MAC（命中即上线，回报其当前 IP）
//     - 再发探测请求填表供下一秒收割：有 ip 提示则 ARP 该 IP；并轮转扫 /24 兜底
//   超时 WAKE_VERIFY_TIMEOUT_MS 回报 ok:false reason:timeout。详见 docs/NET_SCAN.md。
// ═════════════════════════════════════════════════════════════════════════════
#ifndef ARP_TABLE_SIZE
#define ARP_TABLE_SIZE 10
#endif

static bool          wvActive   = false;
static uint8_t       wvTargetMac[6];
static char          wvMacStr[13];
static uint32_t      wvHintIp   = 0;   // 网络字节序，0=无提示
static uint16_t      wvSweepHost = 1;  // 无提示时 /24 轮转游标
static unsigned long wvStart    = 0;
static unsigned long wvLastProbe = 0;

// 回报唤醒验证结果并结束探测
static void wvFinish(bool ok, uint32_t foundIp, const char* reason) {
    StaticJsonDocument<160> doc;
    doc["event"] = "wake_result";
    doc["ok"]    = ok;
    doc["mac"]   = wvMacStr;
    if (ok) {
        doc["ip"]         = IPAddress(foundIp).toString();
        doc["elapsed_ms"] = (unsigned long)(millis() - wvStart);
    } else if (reason) {
        doc["reason"] = reason;
    }
    pubJson(doc);
    Serial.printf("[WV] result ok=%d mac=%s\n", ok, wvMacStr);
    wvActive = false;
}

// 启动唤醒验证。mac 为 12 位 hex（已校验）；ipHint 可空。
static void wvStartVerify(const char* mac, const char* ipHint) {
    strlcpy(wvMacStr, mac, sizeof(wvMacStr));
    for (int i = 0; i < 6; i++) {
        char h[3] = { mac[i * 2], mac[i * 2 + 1], '\0' };
        wvTargetMac[i] = (uint8_t)strtol(h, nullptr, 16);
    }
    wvHintIp = 0;
    if (ipHint && ipHint[0]) {
        IPAddress tmp;
        if (tmp.fromString(ipHint)) wvHintIp = (uint32_t)tmp;
    }
    nsSelfIp = (uint32_t)WiFi.localIP();
    nsBase24 = nsSelfIp & 0x00FFFFFF;
    nsNetif  = nsFindStaNetif();
    wvSweepHost = 1;
    wvStart     = millis();
    wvLastProbe = 0;   // 立即第一轮（先发探测，下一拍收割）
    wvActive    = true;
    Serial.printf("[WV] verify start mac=%s hint=%s\n", wvMacStr,
                  wvHintIp ? IPAddress(wvHintIp).toString().c_str() : "(none)");
}

// 每轮 loop 调用，非阻塞，每秒推进一拍
static void wvTick() {
    if (!wvActive) return;
    unsigned long now = millis();
    if (now - wvStart > WAKE_VERIFY_TIMEOUT_MS) {
        wvFinish(false, 0, "timeout");
        return;
    }
    if (now - wvLastProbe < WAKE_VERIFY_PROBE_MS) return;
    wvLastProbe = now;

    // 1. 收割：扫 lwIP ARP 表，按 MAC 找目标
    uint32_t foundIp = 0;
    LOCK_TCPIP_CORE();
    for (size_t i = 0; i < ARP_TABLE_SIZE; i++) {
        ip4_addr_t*      ip  = nullptr;
        struct netif*    nif = nullptr;
        struct eth_addr* eth = nullptr;
        if (etharp_get_entry(i, &ip, &nif, &eth) && eth && ip) {
            if (memcmp(eth->addr, wvTargetMac, 6) == 0) { foundIp = ip->addr; break; }
        }
    }
    UNLOCK_TCPIP_CORE();
    if (foundIp != 0) {
        wvFinish(true, foundIp, nullptr);
        return;
    }

    // 2. 发探测请求填表（供下一拍收割）
    LOCK_TCPIP_CORE();
    if (wvHintIp != 0) {
        ip4_addr_t a; a.addr = wvHintIp;
        etharp_request(nsNetif, &a);
    }
    for (uint8_t k = 0; k < WAKE_VERIFY_WINDOW; k++) {
        uint32_t ip = nsBase24 | ((uint32_t)wvSweepHost << 24);
        wvSweepHost = (wvSweepHost >= 254) ? 1 : wvSweepHost + 1;
        if (ip == nsSelfIp) continue;
        ip4_addr_t a; a.addr = ip;
        etharp_request(nsNetif, &a);
    }
    UNLOCK_TCPIP_CORE();
}

// ─────────────────────────────────────────────────────────────────────────────
// LED 非阻塞闪烁：blinkStart / blinkStop / blinkTick
//   blinkStart — 启动闪烁，只填充状态，立即返回
//   blinkStop  — 中止闪烁并关灯；LED 其他指令到来时先调用此函数
//   blinkTick  — 驱动函数，在 loop() 末尾每轮调用
// ─────────────────────────────────────────────────────────────────────────────
void blinkStart(int times, int intervalMs) {
    blink.active       = true;
    blink.remaining    = times * 2;          // 每次闪烁 = 亮 + 灭，各翻转一次
    blink.intervalMs   = intervalMs;
    blink.nextToggleMs = millis();           // 立即开始第一次翻转
}

void blinkStop() {
    blink.active = false;
    digitalWrite(LED_PIN, HIGH);             // 关灯（active LOW）
}

void blinkTick() {
    if (!blink.active) return;
    if (millis() < blink.nextToggleMs) return;

    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
    blink.remaining--;
    blink.nextToggleMs = millis() + blink.intervalMs;

    if (blink.remaining <= 0) blinkStop();
}

// ─────────────────────────────────────────────────────────────────────────────
// doOTA：从指定 URL 下载固件并执行 OTA 升级
//   流程：发布 ota_start → 断开 MQTT（释放 TLS 内存）→ 下载写入 → 重连 MQTT →
//         发布 ota_success（后立即重启）或 ota_fail（恢复正常运行）
//   重定向：手动逐跳跟踪（每跳独立新 WiFiClient/WiFiClientSecure），支持跨主机
//   HTTP/HTTPS：根据每跳 URL scheme 自动选择客户端类型，TLS 不验证证书
// ─────────────────────────────────────────────────────────────────────────────
void doOTA(const char* urlParam) {
    // 立即复制 URL，避免后续操作污染调用方的栈帧或 MQTT 缓冲区
    char url[512];
    strlcpy(url, urlParam, sizeof(url));

    Serial.printf("[OTA] starting: %s\n", url);

    // 1. 发布 ota_start（此时 MQTT 仍连接）
    {
        StaticJsonDocument<320> doc;
        doc["event"]  = "ota_start";
        doc["url"]    = url;
        doc["uptime"] = millis() / 1000;
        doc["heap"]   = ESP.getFreeHeap();
        char buf[320];
        serializeJson(doc, buf, sizeof(buf));
        mqtt.publish(TOPIC_PUB, buf);
    }
    delay(300);  // 等待 MQTT 发送完成

    // 2. 断开 MQTT，释放 TLS 上下文内存
    mqtt.disconnect();
    delay(100);

    bool   success = false;
    String failReason;

    // ── HTTPClient + Update，手动逐跳跟踪重定向，每跳独立新客户端 ──────────────
    // 不使用 setFollowRedirects：
    //   - HTTPC_FORCE_FOLLOW_REDIRECTS 在跨主机跳转时复用同一 TLS 对象，导致握手失败
    // 统一实现：每跳新建 WiFiClient/WiFiClientSecure，确保 TLS 连接独立
    {
        String currentUrl = String(url);
        for (int hop = 0; hop < 5; hop++) {
            bool hopHttps = currentUrl.startsWith("https://");

            // 两种客户端均在循环体内声明，生命周期覆盖本次跳转（含流式写入）
            WiFiClientSecure sc;
            WiFiClient       c;

            HTTPClient http;
            http.setTimeout(30000);
            http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);

            bool began;
            if (hopHttps) {
                sc.setInsecure();
                began = http.begin(sc, currentUrl);
            } else {
                began = http.begin(c, currentUrl);
            }
            if (!began) {
                failReason = F("http.begin failed");
                break;
            }

            int code = http.GET();
            Serial.printf("[OTA] hop %d HTTP %d\n", hop, code);

            if ((code / 100) == 3) {
                String loc = http.getLocation();
                http.end();
                if (loc.length() > 0) {
                    Serial.printf("[OTA] → %s\n", loc.c_str());
                    currentUrl = loc;
                    continue;
                }
                failReason = F("redirect: no Location header");
                break;
            }

            if (code == HTTP_CODE_OK) {
                int totalSize = http.getSize();
                Serial.printf("[OTA] size: %d bytes\n", totalSize);
                int updateSize = totalSize > 0 ? totalSize : UPDATE_SIZE_UNKNOWN;
                if (!Update.begin(updateSize)) {
                    failReason = Update.errorString();
                    Serial.printf("[OTA] Update.begin failed: %s\n", failReason.c_str());
                } else {
                    Update.onProgress([](size_t done, size_t total) {
                        if (total > 0 && done % (total / 10 + 1) == 0)
                            Serial.printf("[OTA] %u%%\n", (unsigned)(done * 100 / total));
                    });
                    WiFiClient* stream = http.getStreamPtr();
                    stream->setTimeout(30000);
                    size_t written = Update.writeStream(*stream);
                    Serial.printf("[OTA] written %u bytes\n", (unsigned)written);
                    if (Update.end(true) && Update.isFinished()) {
                        success = true;
                    } else {
                        failReason = Update.errorString();
                        Serial.printf("[OTA] Update.end failed: %s\n", failReason.c_str());
                    }
                }
            } else if (code < 0) {
                failReason = String(F("HTTP error: ")) + HTTPClient::errorToString(code);
                Serial.printf("[OTA] connection error: %s\n", failReason.c_str());
            } else {
                failReason = String(F("HTTP ")) + code;
                Serial.printf("[OTA] unexpected response: %d\n", code);
            }
            http.end();
            break;
        }
    }

    // 3. 重连 MQTT，发布结果
    Serial.printf("[OTA] result: %s\n", success ? "success" : failReason.c_str());
    connectMQTT();

    if (success) {
        char buf[64];
        snprintf(buf, sizeof(buf),
            "{\"event\":\"ota_success\",\"uptime\":%lu,\"heap\":%u}",
            millis() / 1000, ESP.getFreeHeap());
        mqtt.publish(TOPIC_PUB, buf);
    } else {
        StaticJsonDocument<256> doc;
        doc["event"]  = "ota_fail";
        doc["reason"] = failReason;
        doc["uptime"] = millis() / 1000;
        doc["heap"]   = ESP.getFreeHeap();
        char buf[256];
        serializeJson(doc, buf, sizeof(buf));
        mqtt.publish(TOPIC_PUB, buf);
    }
    delay(500);
    ESP.restart();
}

// ─────────────────────────────────────────────────────────────────────────────
// doSpeedtest：HTTP 下载测速，MQTT 全程保持连接
//   流程：heap 检查 → 发布 speedtest_start → HTTP GET（仅 HTTP，拒绝 HTTPS）
//         → 丢弃数据计字节，超时或 EOF 停止 → 发布 speedtest_result / speedtest_fail
//   计时从收到首字节开始，排除 TCP 握手和 HTTP 头部解析耗时
//   读取使用 512B 栈 buffer，不在 heap 分配接收缓冲区
// ─────────────────────────────────────────────────────────────────────────────
void doSpeedtest(const char* urlParam, int maxSeconds) {
    maxSeconds = constrain(maxSeconds, 5, 60);

    if ((int)ESP.getMaxAllocHeap() < SPEEDTEST_MIN_HEAP) {
        pubEvent("error:heap_low");
        return;
    }

    char url[256];
    strlcpy(url, urlParam, sizeof(url));

    Serial.printf("[SPD] start: %s  max=%ds\n", url, maxSeconds);
    {
        char buf[128];
        snprintf(buf, sizeof(buf),
            "{\"event\":\"speedtest_start\",\"max_seconds\":%d,\"uptime\":%lu,\"heap\":%u}",
            maxSeconds, millis() / 1000, ESP.getFreeHeap());
        mqtt.publish(TOPIC_PUB, buf);
    }

    bool     success    = false;
    String   failReason;
    uint32_t totalBytes = 0;
    uint32_t elapsedMs  = 0;

    {
        String currentUrl = String(url);
        for (int hop = 0; hop < 5; hop++) {
            if (currentUrl.startsWith("https://")) {
                failReason = F("https not supported");
                break;
            }

            WiFiClient c;
            HTTPClient http;
            http.setTimeout(10000);
            http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);

            if (!http.begin(c, currentUrl)) {
                failReason = F("http.begin failed");
                break;
            }

            int code = http.GET();
            Serial.printf("[SPD] hop %d HTTP %d\n", hop, code);

            if ((code / 100) == 3) {
                String loc = http.getLocation();
                http.end();
                if (loc.length() > 0) {
                    Serial.printf("[SPD] → %s\n", loc.c_str());
                    currentUrl = loc;
                    continue;
                }
                failReason = F("redirect: no Location");
                break;
            }

            if (code == HTTP_CODE_OK) {
                WiFiClient* stream = http.getStreamPtr();
                stream->setTimeout(5000);

                uint8_t  buf[512];
                uint32_t startMs  = millis();
                uint32_t deadline = startMs + (uint32_t)maxSeconds * 1000;

                while (millis() < deadline) {
                    if (stream->available()) {
                        int n = stream->read(buf, sizeof(buf));
                        if (n > 0) totalBytes += (uint32_t)n;
                        else break;
                    } else {
                        if (!stream->connected()) break;
                        yield();
                    }
                }

                elapsedMs = millis() - startMs;
                if (totalBytes > 0 && elapsedMs > 0) {
                    success = true;
                } else {
                    failReason = F("no data received");
                }
                Serial.printf("[SPD] %u bytes in %u ms\n", totalBytes, elapsedMs);
            } else if (code < 0) {
                failReason = String(F("HTTP error: ")) + HTTPClient::errorToString(code);
                Serial.printf("[SPD] error: %s\n", failReason.c_str());
            } else {
                failReason = String(F("HTTP ")) + code;
            }

            http.end();
            break;
        }
    }

    if (success) {
        uint32_t kbps = totalBytes * 8 / elapsedMs;
        StaticJsonDocument<192> doc;
        doc["event"]      = "speedtest_result";
        doc["bytes"]      = totalBytes;
        doc["elapsed_ms"] = elapsedMs;
        doc["kbps"]       = kbps;
        doc["uptime"]     = millis() / 1000;
        doc["heap"]       = ESP.getFreeHeap();
        char buf[192];
        serializeJson(doc, buf, sizeof(buf));
        mqtt.publish(TOPIC_PUB, buf);
    } else {
        StaticJsonDocument<192> doc;
        doc["event"]  = "speedtest_fail";
        doc["reason"] = failReason;
        doc["uptime"] = millis() / 1000;
        doc["heap"]   = ESP.getFreeHeap();
        char buf[192];
        serializeJson(doc, buf, sizeof(buf));
        mqtt.publish(TOPIC_PUB, buf);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// doSetMqtt：在线更新 MQTT 连接配置
//   流程：断开当前 MQTT → 临时客户端测试新连接 → 成功则保存并重启，失败则恢复原连接
//   注意：测试客户端与主客户端顺序使用，避免双 TLS 上下文同时占用内存
//   重启原因：保证 TLS 状态干净、mqttEverConnected 复位、新 broker 收到 online 事件
// ─────────────────────────────────────────────────────────────────────────────
void doSetMqtt(const char* newServer, uint16_t newPort,
               const char* newUser,   const char* newPass) {
    // 1. 用当前配置作基础，仅覆盖非空字段（mqtt_id 由芯片派生，不可变）
    char   testServer[64];  strlcpy(testServer, strlen(newServer) > 0 ? newServer : cfg.mqtt_server, sizeof(testServer));
    uint16_t testPort =     newPort > 0       ? newPort   : cfg.mqtt_port;
    char   testUser[64];    strlcpy(testUser,   strlen(newUser)   > 0 ? newUser   : cfg.mqtt_user,   sizeof(testUser));
    char   testPass[64];    strlcpy(testPass,   strlen(newPass)   > 0 ? newPass   : cfg.mqtt_pass,   sizeof(testPass));

    Serial.printf("[SET_MQTT] testing → %s:%u  user=%s  id=%s\n",
        testServer, testPort, testUser, cfg.mqtt_id);

    // 2. 断开当前 MQTT（释放 TLS 资源）
    // 失败时直接用 cfg.* 回退（cfg 在失败分支中未被修改）
    // 先清除旧 broker 上的 retained 消息（空 payload + retain=true）
    // mqtt.disconnect() 发送正常 DISCONNECT 包，不会触发 LWT，retained online 会残留
    if (mqtt.connected()) {
        mqtt.publish(TOPIC_PUB, (const uint8_t*)"", 0, true);
        delay(100);
    }
    mqtt.disconnect();
    delay(100);

    // 3. 用独立临时客户端（独立 TLS 上下文）测试连接
    bool ok = false;
    int  testRc = 0;
    {
        WiFiClientSecure testWifiClient;
        testWifiClient.setInsecure();
        PubSubClient testMqtt(testWifiClient);
        testMqtt.setServer(testServer, testPort);
        testMqtt.setSocketTimeout(10);  // 单次 TCP 超时 10s，避免长时间阻塞

        Serial.println(F("[SET_MQTT] connecting test client..."));
        ok = testMqtt.connect(cfg.mqtt_id, testUser, testPass);
        testRc = testMqtt.state();
        if (ok) {
            testMqtt.disconnect();
            Serial.println(F("[SET_MQTT] test OK"));
        } else {
            Serial.printf("[SET_MQTT] test FAILED rc=%d\n", testRc);
        }
        // testWifiClient 析构，TLS 资源释放
    }

    if (ok) {
        // 4a. 写入新配置、持久化、重启
        // 主客户端已在步骤 2 断开，无法在此 publish；重启后设备用新配置连接新 broker
        // 并发出 online 事件，作为切换成功的隐式确认
        strlcpy(cfg.mqtt_server, testServer, sizeof(cfg.mqtt_server));
        cfg.mqtt_port = testPort;
        strlcpy(cfg.mqtt_user, testUser, sizeof(cfg.mqtt_user));
        strlcpy(cfg.mqtt_pass, testPass, sizeof(cfg.mqtt_pass));
        saveConfig();
        Serial.println(F("[SET_MQTT] config saved, rebooting"));
        ESP.restart();

    } else {
        // 4b. cfg 未变，重连原 broker 发出错误事件后重启
        Serial.printf("[SET_MQTT] test FAILED rc=%d, rebooting\n", testRc);
        mqtt.setServer(cfg.mqtt_server, cfg.mqtt_port);
        if (mqtt.connect(cfg.mqtt_id, cfg.mqtt_user, cfg.mqtt_pass,
                TOPIC_PUB, 0, true, "{\"event\":\"offline\"}")) {
            char buf[128];
            snprintf(buf, sizeof(buf),
                "{\"event\":\"error:mqtt_connect_failed\",\"rc\":%d,\"uptime\":%lu,\"heap\":%u}",
                testRc, millis() / 1000, ESP.getFreeHeap());
            mqtt.publish(TOPIC_PUB, buf);
            delay(200);
        }
        ESP.restart();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// mqttCallback：处理下行指令
// ─────────────────────────────────────────────────────────────────────────────
void mqttCallback(char* topic, byte* payload, unsigned int len) {
    // OTA 命令为最大入参（URL≤511 + JSON开销~30 ≈ 541），640 有足够余量
    char msg[640] = {};
    if (len >= sizeof(msg)) len = sizeof(msg) - 1;
    memcpy(msg, payload, len);
    int end = (int)strlen(msg) - 1;
    while (end >= 0 && (msg[end] == ' ' || msg[end] == '\n' || msg[end] == '\r'))
        msg[end--] = '\0';

    Serial.printf("[MQTT] ← %s\n", msg);
    if (strlen(msg) == 0) return;
    if (msg[0] != '{') return;

    StaticJsonDocument<512> doc;
    if (deserializeJson(doc, msg) != DeserializationError::Ok) {
        pubEvent("error:json_parse");
        return;
    }

    const char* cmd = doc["cmd"] | "";

    // ── wol ──
    if (strcmp(cmd, "wol") == 0) {
        const char* mac_param = doc["mac"] | "";
        const char* target_mac = cfg.wol_mac;
        if (strlen(mac_param) > 0) {
            bool hexOk = (strlen(mac_param) == 12);
            for (int i = 0; hexOk && i < 12; i++) hexOk = isxdigit((unsigned char)mac_param[i]);
            if (!hexOk) { pubEvent("error:invalid_mac"); return; }
            target_mac = mac_param;
        }
        // 设备未存默认 MAC 且指令未带 mac → 拒绝（用户应在手机端指定目标）
        if (strlen(target_mac) != 12) { pubEvent("error:no_mac"); return; }
        sendMagicPacket(target_mac);
        StaticJsonDocument<96> resp;
        resp["event"] = "wol";
        resp["mac"]   = target_mac;
        pubJson(resp);
        // verify:true → 发包后进入唤醒验证探测（异步，回报 wake_result）
        if (doc["verify"] | false) {
            wvStartVerify(target_mac, doc["ip"] | "");
        }

    // ── ping ──
    } else if (strcmp(cmd, "ping") == 0) {
        pubEvent("pong");

    // ── reboot ──
    } else if (strcmp(cmd, "reboot") == 0) {
        pubReboot("cmd");

    // ── info ──
    } else if (strcmp(cmd, "info") == 0) {
        StaticJsonDocument<256> resp;
        resp["event"]      = "info";
        resp["version"]    = FW_VERSION;
        resp["ssid"]       = cfg.wifi_ssid;
        resp["rssi"]       = WiFi.RSSI();
        resp["wol_mac"]    = cfg.wol_mac;
        resp["reconnects"] = mqttReconnectCount;
        pubJson(resp);  // appends uptime / heap / ip

    // ── config ──
    } else if (strcmp(cmd, "config") == 0) {
        StaticJsonDocument<640> resp;
        resp["event"]           = "config";
        resp["wifi_ssid"]       = cfg.wifi_ssid;
        JsonArray nets = resp.createNestedArray("wifi_networks");
        for (uint8_t i = 0; i < wifiCount; i++) nets.add(lruGet(i)->ssid);
        resp["mqtt_server"]     = cfg.mqtt_server;
        resp["mqtt_port"]       = cfg.mqtt_port;
        resp["mqtt_user"]       = cfg.mqtt_user;
        resp["mqtt_id"]         = cfg.mqtt_id;
        resp["wol_mac"]         = cfg.wol_mac;
        resp["status_interval"] = statusIntervalMs / 1000;
        resp["wifi_tx_power"]   = cfg.wifi_tx_power;
        pubJson(resp);  // appends uptime / heap / ip

    // ── set ──
    } else if (strcmp(cmd, "set") == 0) {
        const char* key = doc["key"] | "";

        if (strcmp(key, "status_interval") == 0) {
            int val = doc["val"] | -1;
            if (val < 0) {
                pubEvent("error:invalid_value");
            } else if (val == 0) {
                statusIntervalMs = 0;
                pubEvent("ok:status_disabled");
            } else if (val < STATUS_INTERVAL_MIN) {
                pubEvent("error:status_interval_too_short");
            } else if (val > STATUS_INTERVAL_MAX) {
                pubEvent("error:status_interval_too_long");
            } else {
                statusIntervalMs = (unsigned long)val * 1000;
                pubEvent("ok:status_interval_updated");
            }
        } else {
            pubEvent("error:unknown_key");
        }

    // ── led ──
    } else if (strcmp(cmd, "led") == 0) {
        const char* val = doc["val"] | "";

        if (strcmp(val, "on") == 0) {
            blinkStop();
            digitalWrite(LED_PIN, LOW);
            pubEvent("led:on");
        } else if (strcmp(val, "off") == 0) {
            blinkStop();
            digitalWrite(LED_PIN, HIGH);
            pubEvent("led:off");
        } else if (strcmp(val, "toggle") == 0) {
            blinkStop();
            digitalWrite(LED_PIN, !digitalRead(LED_PIN));
            pubEvent("led:toggle");
        } else if (strcmp(val, "query") == 0) {
            // blink 进行中时 LED 状态不稳定，直接报告 blink 状态更准确
            if (blink.active) {
                pubEvent("led:blink");
            } else {
                pubEvent(digitalRead(LED_PIN) == LOW ? "led:on" : "led:off");
            }
        } else if (strcmp(val, "blink") == 0) {
            int times    = constrain((int)(doc["times"]    | 5), 1, 10);
            int interval = constrain((int)(doc["interval"] | 500), 100, 2000);
            blinkStart(times, interval);
            pubEvent("led:blink");
        } else {
            pubEvent("error:unknown_led_val");
        }

    // ── set_mqtt ──
    } else if (strcmp(cmd, "set_mqtt") == 0) {
        const char* newServer = doc["server"] | "";
        uint16_t    newPort   = doc["port"]   | 0;
        const char* newUser   = doc["user"]   | "";
        const char* newPass   = doc["pass"]   | "";

        // 至少要有一个字段发生实质性变化
        bool anyChange = (strlen(newServer) > 0 && strcmp(newServer, cfg.mqtt_server) != 0)
                      || (newPort > 0            && newPort != cfg.mqtt_port)
                      || (strlen(newUser) > 0    && strcmp(newUser, cfg.mqtt_user) != 0)
                      || (strlen(newPass) > 0    && strcmp(newPass, cfg.mqtt_pass) != 0);
        if (!anyChange) {
            pubEvent("error:set_mqtt_no_change");
        } else {
            doSetMqtt(newServer, newPort, newUser, newPass);
        }

    // ── ota ──
    } else if (strcmp(cmd, "ota") == 0) {
        const char* url = doc["url"] | "";
        if (strlen(url) == 0) {
            pubEvent("error:ota_url_missing");
        } else {
            doOTA(url);   // 内部处理所有结果上报，成功时直接重启
        }

    // ── speedtest ──
    } else if (strcmp(cmd, "speedtest") == 0) {
        const char* url = doc["url"] | SPEEDTEST_DEFAULT_URL;
        int maxSec      = doc["max_seconds"] | SPEEDTEST_DEFAULT_SECS;
        doSpeedtest(url, maxSec);

    // ── net_scan ──
    } else if (strcmp(cmd, "net_scan") == 0) {
        // 扫描进行中（或 OTA 等占用）再收指令 → busy；结果经 nsTick 异步分批上报
        if (!nsStart(true)) pubEvent("error:net_scan_busy");

    // ── 未知指令 ──
    } else {
        pubEvent("error:unknown_cmd");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// connectWiFi
// ─────────────────────────────────────────────────────────────────────────────
static const char* wifiStatusName(int s) {
    switch (s) {
        case 0: return "WL_IDLE";
        case 1: return "WL_NO_SSID_AVAIL";
        case 2: return "WL_SCAN_COMPLETED";
        case 3: return "WL_CONNECTED";
        case 4: return "WL_CONNECT_FAILED";
        case 5: return "WL_CONNECTION_LOST";
        case 6: return "WL_DISCONNECTED";
        case 7: return "WL_WRONG_PASSWORD";
        case 255: return "WL_NO_SHIELD";
        default:  return "UNKNOWN";
    }
}

// connectWiFiMulti：轮试历史 WiFi 列表，连上后把成功的网络移至列表头部。
// 连接前先扫描,优先尝试当前可见网络。
// 全部失败时：首次连接进 BLE 配网，曾经连上过则重启（SDK 下次启动自动重试）。
void connectWiFiMulti() {
    if (wifiCount == 0) {
        Serial.println(F("[WiFi] no networks in history → BLE provisioning"));
        enterBLEProvMode();  // 永不返回
    }

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);  // 轮试期间手动管理，连上后再开启
    {
        int8_t txp = cfg.wifi_tx_power;
        if (txp == 0) txp = 15;
        if (txp > 0) esp_wifi_set_max_tx_power((int8_t)(txp * 4));
    }
    Serial.printf("[WiFi] TX power = %ddBm%s\n", cfg.wifi_tx_power,
        (cfg.wifi_tx_power == 0) ? " (platform default)" : "");

    // ── 构建候选顺序（LRU 位置索引） ─────────────────────────────────────────
    uint8_t order[WIFI_MAX_NETWORKS];
    uint8_t orderCount = 0;

    // 先扫描，可见网络排前面（隐藏网络仍在列表末尾兜底）
    Serial.println(F("[WiFi] scanning..."));
    int scanN = WiFi.scanNetworks();
    for (uint8_t i = 0; i < wifiCount; i++) {
        for (int j = 0; j < scanN; j++) {
            if (WiFi.SSID(j) == lruGet(i)->ssid) { order[orderCount++] = i; break; }
        }
    }
    WiFi.scanDelete();
    uint8_t visibleCount = orderCount;  // 第一轮结束后保存可见数，第二轮会修改 orderCount
    // 不可见的追加到末尾作为隐藏网络兜底
    for (uint8_t i = 0; i < wifiCount; i++) {
        bool already = false;
        for (uint8_t j = 0; j < orderCount; j++) if (order[j] == i) { already = true; break; }
        if (!already) order[orderCount++] = i;
    }
    Serial.printf("[WiFi] %d visible, %d total candidate(s)\n", visibleCount, wifiCount);

    // ── 逐个尝试 ─────────────────────────────────────────────────────────────
    unsigned long startMs = millis();

    for (uint8_t attempt = 0; attempt < orderCount; attempt++) {
        const WifiNode* n = lruGet(order[attempt]);
        Serial.printf("[WiFi] trying [%d/%d] %s\n", attempt + 1, orderCount, n->ssid);
        WiFi.begin(n->ssid, n->pass);

        unsigned long t = millis();
        bool connected = false;
        int lastStatus = -1;
        while (millis() - t < WIFI_PER_NETWORK_TIMEOUT_MS) {
            delay(500);
            int s = WiFi.status();
            if (s == WL_CONNECTED) { connected = true; break; }
            if (s != lastStatus) {
                Serial.printf("\n  status=%d (%s)", s, wifiStatusName(s));
                lastStatus = s;
            } else {
                Serial.print('.');
            }
            // WL_NO_SSID_AVAIL：SSID 确认不存在，立即跳下一个
            if (s == WL_NO_SSID_AVAIL) {
                Serial.println(F(" → not found, skip"));
                break;
            }
            checkRuntimeTriggers();
        }
        Serial.println();

        if (connected) {
            wifiConnectMs = millis() - startMs;
            Serial.printf("[WiFi] connected to '%s' in %lums, IP=%s\n",
                n->ssid, wifiConnectMs, WiFi.localIP().toString().c_str());

            // 成功的网络移至列表头部并持久化
            // n->ssid/pass 在 lruAddOrUpdate 后仍指向同一池槽，数据不变
            lruAddOrUpdate(n->ssid, n->pass);
            saveWifiNetworks();
            strlcpy(cfg.wifi_ssid, lruGet(0)->ssid, sizeof(cfg.wifi_ssid));
            strlcpy(cfg.wifi_pass, lruGet(0)->pass, sizeof(cfg.wifi_pass));

            WiFi.setAutoReconnect(true);  // 交还给 SDK 处理后续断线重连

            if (!cfg.wifi_ever_ok) {
                cfg.wifi_ever_ok = true;
                if (saveConfig()) Serial.println(F("[CFG] wifi_ever_ok saved"));
                else              Serial.println(F("[CFG] WARNING: wifi_ever_ok save failed"));
            }
            return;
        }

        WiFi.disconnect(true);
        delay(200);
    }

    // ── 全部失败 ─────────────────────────────────────────────────────────────
    Serial.println(F("[WiFi] all networks failed"));
    if (!cfg.wifi_ever_ok) {
        Serial.println(F("[WiFi] first run → BLE provisioning"));
        enterBLEProvMode();  // 永不返回
    } else {
        Serial.println(F("[WiFi] → restart"));
        ESP.restart();
    }
}

// waitWiFiConnected：loop() 掉线时调用，等待 SDK 自动重连到已知网络
// 不再负责首次连接逻辑（由 connectWiFiMulti 处理）
void waitWiFiConnected() {
    if (WiFi.status() == WL_CONNECTED) return;

    unsigned long t = millis();
    int lastStatus = -1;
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        int s = WiFi.status();
        if (s != lastStatus) {
            Serial.printf("\n[WiFi] status=%d (%s)", s, wifiStatusName(s));
            lastStatus = s;
        } else {
            Serial.print('.');
        }
        checkRuntimeTriggers();
        if (millis() - t > WIFI_TIMEOUT_MS) {
            Serial.println(F("\n[WiFi] reconnect timeout → restart"));
            ESP.restart();
        }
    }
    Serial.printf("\n[WiFi] reconnected, IP=%s\n", WiFi.localIP().toString().c_str());
}

// ─────────────────────────────────────────────────────────────────────────────
// connectMQTT
// ─────────────────────────────────────────────────────────────────────────────
void connectMQTT() {
    int retries = 0;
    while (!mqtt.connected()) {
        Serial.printf("[MQTT] connecting (attempt %d/%d)...\n",
            retries + 1, MQTT_RETRY_MAX);

        if (mqtt.connect(cfg.mqtt_id, cfg.mqtt_user, cfg.mqtt_pass,
                TOPIC_PUB, 0, true, "{\"event\":\"offline\"}")) {
            Serial.println(F("[MQTT] connected"));
            mqtt.subscribe(TOPIC_SUB);

            if (!mqttEverConnected) {
                mqttEverConnected = true;
                StaticJsonDocument<128> doc;
                doc["event"]       = "online";
                doc["version"]     = FW_VERSION;
                doc["wifi_conn_ms"] = wifiConnectMs;
                pubJson(doc, true);
            } else {
                mqttReconnectCount++;
                Serial.printf("[MQTT] reconnect #%u\n", mqttReconnectCount);
                StaticJsonDocument<128> doc;
                doc["event"] = "reconnect";
                doc["count"] = mqttReconnectCount;
                pubJson(doc, true);
            }

        } else {
            Serial.printf("[MQTT] failed, rc=%d\n", mqtt.state());
            retries++;
            if (retries >= MQTT_RETRY_MAX) {
                Serial.println(F("[MQTT] max retries → restart"));
                ESP.restart();
            }
            for (int i = 0; i < 10; i++) {
                delay(500);
                checkRuntimeTriggers();
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// setup
// ─────────────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println(F("\n\n====== WoL ESP32-C3 boot ======"));
    Serial.println(F("Firmware: " FW_VERSION));

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, HIGH);  // 关灯（active low）
    pinMode(CFG_RESET_PIN, INPUT_PULLUP);

    // ── 1. 挂载文件系统 ──
    LittleFS.begin(true);  // formatOnFail
    Serial.println(F("[FS] mounted"));

    // ── 2. 加载配置（需先于所有模式检测，确保 cfg 已填充） ──
    bool configOk = loadConfig();

    // ── 2b. 加载 WiFi 历史列表（依赖 cfg.wifi_ssid 做迁移，必须在 loadConfig 之后）──
    bool hasWifi = loadWifiNetworks();

    // ── 2c. 设备身份：派生 mqtt_id、加载/生成 BLE PSK（须先于任何模式检测）──
    genMqttId(cfg.mqtt_id, sizeof(cfg.mqtt_id));
    bleLoadOrCreatePsk();

    // ── 3. 重配置标志检测 → 串口 CLI（隐藏恢复通道）──
    checkReconfigFlag();

    // ── 4. BLE 配网标志检测 → NimBLE GATT 配网 ──
    checkBLEProvFlag();

    // ── 5. 配置校验：无有效配置 → BLE 配网 ──
    if (!configOk || !hasWifi) {
        Serial.println(F("[CFG] no valid config → BLE provisioning"));
        enterBLEProvMode();  // 永不返回
    }

    // ── 6. 填充运行时主题 ──
    snprintf(TOPIC_SUB, sizeof(TOPIC_SUB), "home/wol/%s/cmd",   cfg.mqtt_id);
    snprintf(TOPIC_PUB, sizeof(TOPIC_PUB), "home/wol/%s/event", cfg.mqtt_id);

    // ── 7. 打印启动信息 ──
    Serial.println(F("------- 当前配置 ---------------"));
    Serial.printf("  固件版本   : %s\n",  FW_VERSION);
    Serial.println(F("  [WiFi 历史]"));
    for (uint8_t i = 0; i < wifiCount; i++)
        Serial.printf("    [%d] %s\n", i, lruGet(i)->ssid);
    Serial.println(F("  [MQTT]"));
    Serial.printf("    服务器   : %s:%u\n", cfg.mqtt_server, cfg.mqtt_port);
    Serial.printf("    用户名   : %s\n",  cfg.mqtt_user);
    Serial.printf("    密码     : %s\n",  cfg.mqtt_pass);
    Serial.printf("    Client ID: %s\n",  cfg.mqtt_id);
    Serial.printf("    订阅主题 : %s\n",  TOPIC_SUB);
    Serial.printf("    发布主题 : %s\n",  TOPIC_PUB);
    Serial.println(F("  [WoL]"));
    Serial.printf("    目标 MAC : %s\n",  cfg.wol_mac);
    Serial.println(F("  [行为]"));
    Serial.printf("    状态上报 : %ds\n", STATUS_INTERVAL_DEFAULT);
    Serial.printf("    定时重启 : %luh\n", REBOOT_INTERVAL_MS / 3600000UL);
    Serial.println(F("--------------------------------"));

    // ── 8. 连接 WiFi（轮试历史列表）──
    connectWiFiMulti();

    // TLS 不验证证书（家用场景，传输仍加密）
    wifiClient.setInsecure();

    // NTP 时间同步（UTC+8），异步不阻塞
    // 暂时禁用：代码中所有时间戳均使用 millis()/1000（uptime），无需真实时钟；
    // TLS 使用 setInsecure() 跳过证书验证，也不依赖系统时间。
    // 待引入绝对时间戳功能时再启用。
    // configTime(8 * 3600, 0, "ntp1.aliyun.com", "ntp2.aliyun.com", "pool.ntp.org");

    // ── 9. 连接 MQTT ──
    mqtt.setServer(cfg.mqtt_server, cfg.mqtt_port);
    mqtt.setCallback(mqttCallback);
    mqtt.setKeepAlive(CONF_MQTT_KEEPALIVE);
    mqtt.setSocketTimeout(MQTT_SOCK_TIMEOUT);
    mqtt.setBufferSize(1024);

    connectMQTT();
}

// ─────────────────────────────────────────────────────────────────────────────
// loop：非阻塞主循环（逻辑不变）
// ─────────────────────────────────────────────────────────────────────────────
void loop() {
    // ── 1. WiFi 掉线检测 ──
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println(F("[WiFi] lost, waiting for reconnect..."));
        waitWiFiConnected();
    }

    // ── 2. MQTT 掉线检测 ──
    if (!mqtt.connected()) {
        Serial.println(F("[MQTT] lost, reconnecting..."));
        connectMQTT();
    }

    // ── 3. 处理 MQTT 收发 ──
    mqtt.loop();

    // ── 4. 24h 定时重启 ──
    if (millis() >= REBOOT_INTERVAL_MS) {
        pubReboot("24h");
    }

    // ── 5. 定时心跳（status_interval 为 0 时禁用）──
    if (statusIntervalMs > 0 &&
        millis() - lastStatusMs >= statusIntervalMs) {
        pubEvent("status");
    }

    // ── 6. 运行时触发检测（串口 config 命令 / 按键重置）──
    checkRuntimeTriggers();

    // ── 7. net_scan 状态机推进（非阻塞，空闲时立即返回）──
    nsTick();

    // ── 8. 唤醒验证探测推进（非阻塞，空闲时立即返回）──
    wvTick();

    // ── 9. LED 非阻塞闪烁驱动 ──
    blinkTick();
}
