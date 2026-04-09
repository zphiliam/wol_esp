/**
 * WoL ESP8266/ESP32-C3 — C++ Arduino 版  v2.2
 *
 * 功能：通过 MQTT 远程唤醒局域网内电脑
 * 硬件：ESP-12F（LED GPIO2）/ ESP32-C3 SuperMini（LED GPIO8），均 active LOW
 *
 * 依赖库（Arduino IDE 库管理器安装）：
 *   - PubSubClient  by Nick O'Leary
 *   - ArduinoJson   by Benoit Blanchon
 *
 * 配置：凭据存于 LittleFS /config.json，固件本身不含敏感信息
 *   首次无配置                        → 直接进入串口 CLI
 *   运行时串口输入 "config" + 回车    → 写 /reconfig → 重启 → 串口 CLI
 *   运行时串口输入 "ap" + 回车        → 写 /softap  → 重启 → SoftAP 网页配置
 *   运行时按住 FLASH/BOOT 键 3s 松手 → 写 /softap  → 重启 → SoftAP 网页配置
 *   运行时按住 FLASH/BOOT 键 10s     → 写 /reconfig → 重启 → 串口 CLI（配置保留）
 *   串口 CLI 输入 help 查看命令列表；SoftAP SSID：WoL-Setup-XXXX，IP：192.168.4.1
 *
 * 主题（运行时由 mqtt_id 拼接）：
 *   下行指令  home/wol/{mqtt_id}/cmd
 *   上行事件  home/wol/{mqtt_id}/event
 *
 * 指令列表：
 *   {"cmd":"wol"}                              唤醒电脑
 *   {"cmd":"ping"}                             测试连通性
 *   {"cmd":"reboot"}                           重启设备
 *   {"cmd":"info"}                             查询设备运行状态（version/ssid/rssi/wol_mac/reconnects）
 *   {"cmd":"config"}                           查询完整配置（含 wifi_networks 列表）
 *   {"cmd":"led","val":"on|off|toggle|query"}  控制 LED
 *   {"cmd":"led","val":"blink","times":5,"interval":500}
 *   {"cmd":"set","key":"status_interval","val":30}  设置状态上报间隔（秒，0=禁用）
 *   {"cmd":"ota","url":"https://..."}          OTA 升级（自动跟踪重定向，支持 GitHub release URL）
 */

#if defined(ESP8266)
  #include <ESP8266WiFi.h>
#else
  #include <WiFi.h>
#endif
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <WiFiUdp.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include "config.h"

// ── OTA ──────────────────────────────────────────────────────────────────────
#ifdef ESP8266
  #include <ESP8266HTTPClient.h>
#else
  #include <HTTPClient.h>
  #include <Update.h>
#endif

// ── WebServer + DNS（配置 AP 模式 / Captive Portal） ─────────────────────────
#ifdef ESP8266
  #include <ESP8266WebServer.h>
  #define WolWebServer ESP8266WebServer
#else
  #include <WebServer.h>
  #define WolWebServer WebServer
#endif
#include <DNSServer.h>
#ifndef ESP8266
  #include "esp_wifi.h"   // esp_wifi_set_ps()
#endif

// ── 硬件 ──────────────────────────────────────────────────────────────────────
#if defined(ESP8266)
const uint8_t LED_PIN = 2;   // ESP-12F，active LOW
#else
const uint8_t LED_PIN = 8;   // ESP32-C3 SuperMini，active LOW
#endif

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
    bool     ap_full_config; // SoftAP 网页是否显示完整 MQTT 配置字段
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
WolWebServer     apServer(80);
DNSServer        dnsServer;

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
bool          btnHit3s      = false;   // 已达 3s 阈值（松手触发 WiFi 重置）

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
    doc["mqtt_id"]      = cfg.mqtt_id;
    doc["wol_mac"]       = cfg.wol_mac;
    doc["wifi_ever_ok"]  = cfg.wifi_ever_ok;
    doc["ap_full_config"]  = cfg.ap_full_config;
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
    strlcpy(cfg.mqtt_id,     doc["mqtt_id"]      | "", sizeof(cfg.mqtt_id));
    strlcpy(cfg.wol_mac,     doc["wol_mac"]      | "", sizeof(cfg.wol_mac));
    cfg.wifi_ever_ok   = doc["wifi_ever_ok"]   | false;
    cfg.ap_full_config = doc["ap_full_config"] | false;
    // 默认值：ESP32-C3 SuperMini 天线设计缺陷+LDO电流不足，15dBm 最稳定（ESP3D 实测推荐）；ESP8266 无此问题用满功率
#ifdef ESP8266
    cfg.wifi_tx_power  = doc["wifi_tx_power"]  | 20;
#else
    cfg.wifi_tx_power  = doc["wifi_tx_power"]  | 15;
#endif

    // 必填项校验（wifi_ssid 已迁移至 wifi_networks.json，此处不再要求）
    return strlen(cfg.mqtt_server) > 0
        && strlen(cfg.mqtt_id)     > 0
        && strlen(cfg.wol_mac)     == 12;
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
    Serial.println(F("  set mqtt_id     <value>"));
    Serial.println(F("  set wol_mac        <value>  (12 hex chars, no separators)"));
    Serial.println(F("  set ap_full_config <true|false>  (show MQTT fields in SoftAP page)"));
    Serial.println(F("  set wifi_tx_power  <2-20|0>      (dBm; 0=platform default; ESP32-C3 推荐 15)"));
    Serial.println(F("  show   - display staged config"));
    Serial.println(F("  save   - write to flash and reboot"));
    Serial.println(F("  ap     - write /softap flag and reboot into SoftAP mode"));
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
        // ESP8266 软件看门狗（SWD）约 3s 超时，此循环永不返回，必须手动喂狗。
        // yield() 在 ESP8266 上触发后台任务调度并重置 SWD；ESP32-C3 上为空操作，无副作用。
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
                Serial.printf("  id        : %s\n",    staged.mqtt_id);
                Serial.println(F("[Device]"));
                Serial.printf("  wol_mac   : %s\n",  staged.wol_mac);
                Serial.printf("  tx_power  : %ddBm%s\n", staged.wifi_tx_power, staged.wifi_tx_power == 0 ? " (default)" : "");
                Serial.printf("  ap_full   : %s\n",   staged.ap_full_config ? "true" : "false");
                Serial.println(F("---------------------------"));

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
                if (staged.mqtt_id[0]     == '\0') { Serial.println(F("[CFG] error: mqtt_id required"));     ok = false; }
                { bool hexOk = (strlen(staged.wol_mac) == 12);
                  for (int i = 0; hexOk && i < 12; i++) hexOk = isxdigit((unsigned char)staged.wol_mac[i]);
                  if (!hexOk) { Serial.println(F("[CFG] error: wol_mac must be 12 hex chars")); ok = false; } }
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

            } else if (line == "ap") {
                if (!LittleFS.exists("/config.json")) {
                    memset(&cfg, 0, sizeof(cfg));
                    cfg.ap_full_config = true;
                    cfg.mqtt_port = 8883;
                    saveConfig();
                    Serial.println(F("[CFG] no config.json → wrote minimal config with ap_full_config=true"));
                }
                writeFlag("/softap");
                Serial.println(F("[CFG] /softap flag written, rebooting into SoftAP..."));
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
                    else if (key == "mqtt_id")     strlcpy(staged.mqtt_id,     val.c_str(), sizeof(staged.mqtt_id));
                    else if (key == "mqtt_port") {
                        int p = val.toInt();
                        if (p > 0 && p <= 65535) staged.mqtt_port = (uint16_t)p;
                        else { Serial.println(F("[CFG] error: port must be 1-65535")); valid = false; }
                    } else if (key == "wol_mac") {
                        bool hexOk = (val.length() == 12);
                        for (int i = 0; hexOk && i < 12; i++) hexOk = isxdigit((unsigned char)val[i]);
                        if (!hexOk) { Serial.println(F("[CFG] error: wol_mac must be 12 hex chars")); valid = false; }
                        else strlcpy(staged.wol_mac, val.c_str(), sizeof(staged.wol_mac));
                    } else if (key == "ap_full_config") {
                        staged.ap_full_config = (val == "true" || val == "1");
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

#include "ap_html.h"

// ─────────────────────────────────────────────────────────────────────────────
// SoftAP 网页处理器
// ─────────────────────────────────────────────────────────────────────────────
static void handleAPConfig() {
    StaticJsonDocument<512> doc;
    doc["ssid"]           = cfg.wifi_ssid;
    doc["wpass"]          = cfg.wifi_pass;
    doc["wolmac"]         = cfg.wol_mac;
    doc["ap_full_config"] = cfg.ap_full_config;
    doc["version"]        = FW_VERSION;
    doc["mqtt_server"]    = cfg.mqtt_server;
    doc["mqtt_port"]      = cfg.mqtt_port;
    doc["mqtt_user"]      = cfg.mqtt_user;
    doc["mqtt_pass"]      = cfg.mqtt_pass;
    doc["mqtt_id"]        = cfg.mqtt_id;
    String json;
    serializeJson(doc, json);
    apServer.send(200, "application/json", json);
}

static void handleAPScan() {
    // scanNetworks() 默认已按 RSSI 降序排列（信号最强在前）
    int n = WiFi.scanNetworks();
    String json = "[";
    for (int i = 0; i < n; i++) {
        if (i > 0) json += ",";
        String ssid = WiFi.SSID(i);
        ssid.replace("\\", "\\\\");
        ssid.replace("\"", "\\\"");
        int rssi = WiFi.RSSI(i);
        json += "{\"ssid\":\"" + ssid + "\",\"rssi\":" + String(rssi) + "}";
    }
    json += "]";
    WiFi.scanDelete();
    apServer.send(200, "application/json", json);
}

static void handleAPRoot() {
    apServer.send(200, "text/html", FPSTR(CONFIG_HTML));
}

static void handleAPSave() {
    String ssid   = apServer.arg("ssid");   ssid.trim();
    String wpass  = apServer.arg("wpass");
    String wolmac = apServer.arg("wolmac"); wolmac.trim();

    // 基础校验
    String err = "";
    if (ssid.length() == 0) {
        err = "WiFi SSID 不能为空。";
    } else if (wolmac.length() != 12) {
        err = "WoL MAC 地址必须是 12 位十六进制字符（无分隔符）。";
    } else {
        bool hexOk = true;
        for (int i = 0; i < 12; i++) hexOk = hexOk && isxdigit((unsigned char)wolmac[i]);
        if (!hexOk) err = "WoL MAC 地址包含非十六进制字符。";
    }

    // ap_full_config 时校验 MQTT 必填项
    String mqttsvr, mqttid;
    if (err.length() == 0 && cfg.ap_full_config) {
        mqttsvr = apServer.arg("mqttsvr"); mqttsvr.trim();
        mqttid  = apServer.arg("mqttid");  mqttid.trim();
        if (mqttsvr.length() == 0) err = "MQTT 服务器地址不能为空。";
        else if (mqttid.length() == 0) err = "MQTT Client ID 不能为空。";
    }

    if (err.length() > 0) {
        StaticJsonDocument<192> errDoc;
        errDoc["ok"]    = false;
        errDoc["error"] = err;
        String errJson;
        serializeJson(errDoc, errJson);
        apServer.send(400, "application/json", errJson);
        return;
    }

    // 写入 cfg 并更新 WiFi 历史列表
    lruAddOrUpdate(ssid.c_str(), wpass.c_str());
    saveWifiNetworks();
    strlcpy(cfg.wifi_ssid, lruGet(0)->ssid, sizeof(cfg.wifi_ssid));
    strlcpy(cfg.wifi_pass, lruGet(0)->pass, sizeof(cfg.wifi_pass));
    strlcpy(cfg.wol_mac,   wolmac.c_str(), sizeof(cfg.wol_mac));
    cfg.wifi_ever_ok = false;

    if (cfg.ap_full_config) {
        strlcpy(cfg.mqtt_server, mqttsvr.c_str(), sizeof(cfg.mqtt_server));
        int port = apServer.arg("mqttport").toInt();
        cfg.mqtt_port = (port > 0 && port <= 65535) ? (uint16_t)port : 8883;
        { String v = apServer.arg("mqttuser"); v.trim(); strlcpy(cfg.mqtt_user, v.c_str(), sizeof(cfg.mqtt_user)); }
        strlcpy(cfg.mqtt_pass, apServer.arg("mqttpass").c_str(), sizeof(cfg.mqtt_pass));
        strlcpy(cfg.mqtt_id,   mqttid.c_str(),  sizeof(cfg.mqtt_id));
    }

    saveConfig();
    Serial.println(F("[AP] config saved, rebooting..."));

    apServer.send(200, "application/json", F("{\"ok\":true}"));
    delay(1500);
    ESP.restart();
}

// ─────────────────────────────────────────────────────────────────────────────
// enterSoftAPMode：启动 SoftAP + HTTP 配置服务器，永不返回
// ─────────────────────────────────────────────────────────────────────────────
void enterSoftAPMode() {
    // 计算 AP SSID（chip ID 末 4 位十六进制，两平台均可）
    char apSSID[20];
#ifdef ESP8266
    snprintf(apSSID, sizeof(apSSID), "WoL-Setup-%04X", (uint16_t)(ESP.getChipId() & 0xFFFF));
#else
    snprintf(apSSID, sizeof(apSSID), "WoL-Setup-%04X", (uint16_t)(ESP.getEfuseMac() & 0xFFFF));
#endif

    // ESP32-C3：彻底停掉 STA，关闭省电模式（否则 beacon 不广播）
    // WIFI_AP_STA：保留 STA 射频，使 scanNetworks() 可在 AP 运行期间使用
    WiFi.disconnect(true);
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_AP_STA);
    delay(200);
#ifndef ESP8266
    esp_wifi_set_ps(WIFI_PS_NONE);   // 关闭省电，确保 beacon 持续广播
#endif
    bool apOk = WiFi.softAP(apSSID, nullptr, 6);  // 指定 ch6，避免信道扫描遗漏
#ifndef ESP8266
    {
        int8_t txp = cfg.wifi_tx_power;
#ifdef CONFIG_IDF_TARGET_ESP32C3
        if (txp == 0) txp = 15;  // ESP32-C3 天线缺陷+LDO不足，默认限制 15dBm
#endif
        if (txp > 0) esp_wifi_set_max_tx_power((int8_t)(txp * 4));
    }
#endif
    Serial.printf("[AP] softAP() = %s\n", apOk ? "OK" : "FAILED");
    Serial.printf("\n[AP] SoftAP started — SSID: %s  IP: 192.168.4.1\n", apSSID);
    Serial.println(F("[AP] Connect to the AP, then open http://192.168.4.1"));
    Serial.printf("[AP] ap_full_config = %s\n", cfg.ap_full_config ? "true (all fields)" : "false (wifi+mac only)");

    apServer.on("/",       HTTP_GET,  handleAPRoot);
    apServer.on("/config", HTTP_GET,  handleAPConfig);
    apServer.on("/scan",   HTTP_GET,  handleAPScan);
    apServer.on("/save",   HTTP_POST, handleAPSave);
    apServer.onNotFound([]() {
        apServer.sendHeader("Location", "http://192.168.4.1/");
        apServer.send(302, "text/plain", "");
    });
    apServer.begin();

    // DNS 劫持：所有域名解析到本机，触发系统 captive portal 检测
    dnsServer.start(53, "*", WiFi.softAPIP());

    unsigned long lastBlink = 0;
    bool ledOn = false;

    while (true) {
        dnsServer.processNextRequest();
        apServer.handleClient();
        checkRuntimeTriggers();   // 按键 10s 仍可触发 /reconfig → restart

        // LED 500ms 慢闪（区别于串口 CLI 的 100ms 快闪）
        if (millis() - lastBlink >= 500) {
            ledOn = !ledOn;
            digitalWrite(LED_PIN, ledOn ? LOW : HIGH);
            lastBlink = millis();
        }
        yield();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// checkSoftAPFlag：检测 /softap 标志，有则进入 SoftAP 配置模式
// ─────────────────────────────────────────────────────────────────────────────
void checkSoftAPFlag() {
    if (!LittleFS.exists("/softap")) return;
    LittleFS.remove("/softap");
    Serial.println(F("[CFG] /softap flag → SoftAP mode"));
    enterSoftAPMode();  // 永不返回
}

// ─────────────────────────────────────────────────────────────────────────────
// checkRuntimeTriggers：可在任意循环中调用，非阻塞
//   串口输入 "config"  → 写 /reconfig，重启进入串口 CLI
//   串口输入 "ap"      → 写 /softap，重启进入 SoftAP 模式
//   串口输入 "reboot"  → 直接重启
//   串口输入 "show"    → 打印当前运行状态（不重启）
//   按键按住 3s 松手   → 写 /softap，重启进入 SoftAP 模式（LED 双闪提示）
//   按键按住 10s      → 写 /reconfig，重启进入串口 CLI（配置保留，LED 五闪确认）
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
            } else if (serialLineBuf == "ap") {
                if (!LittleFS.exists("/config.json")) {
                    memset(&cfg, 0, sizeof(cfg));
                    cfg.ap_full_config = true;
                    cfg.mqtt_port = 8883;
                    saveConfig();
                    Serial.println(F("[CFG] no config.json → wrote minimal config with ap_full_config=true"));
                }
                writeFlag("/softap");
                Serial.println(F("[CFG] serial trigger → writing /softap, rebooting into SoftAP..."));
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
        Serial.println(F("[CFG] button held — 3s: SoftAP, 10s: serial CLI (config preserved)"));

    } else if (btnDown && btnPressing) {
        if (elapsed >= 10000) {
            // 长按 10s：写 /reconfig 进串口 CLI（配置保留），LED 五闪确认
            Serial.println(F("[CFG] 10s → /reconfig flag, rebooting into serial CLI..."));
            writeFlag("/reconfig");
            for (int i = 0; i < 10; i++) {
                digitalWrite(LED_PIN, i % 2 == 0 ? LOW : HIGH);
                delay(100);
            }
            digitalWrite(LED_PIN, HIGH);
            ESP.restart();
        } else if (elapsed >= 3000 && !btnHit3s) {
            // 刚跨过 3s 阈值：LED 双闪提示可松手进 SoftAP
            btnHit3s = true;
            Serial.println(F("[CFG] release for SoftAP, keep 10s for serial CLI"));
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
            // 松手在 3-10s 之间：写 /softap 标志，重启进 SoftAP 模式
            Serial.println(F("[CFG] 3s release → /softap flag, rebooting into SoftAP..."));
            writeFlag("/softap");
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

    // 2. 断开 MQTT，释放 TLS 上下文内存（ESP8266 关键步骤）
    mqtt.disconnect();
    delay(100);

    bool   success = false;
    String failReason;

    // ── HTTPClient + Update，手动逐跳跟踪重定向，每跳独立新客户端 ──────────────
    // 不使用 ESPhttpUpdate / setFollowRedirects：
    //   - ESPhttpUpdate 发送 x-ESP8266-* 请求头，部分代理服务会据此屏蔽
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
#ifdef ESP8266
                size_t updateSize = totalSize > 0 ? (size_t)totalSize : (size_t)-1;
                #define OTA_ERROR_STRING Update.getErrorString()
#else
                int updateSize = totalSize > 0 ? totalSize : UPDATE_SIZE_UNKNOWN;
                #define OTA_ERROR_STRING Update.errorString()
#endif
                if (!Update.begin(updateSize)) {
                    failReason = OTA_ERROR_STRING;
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
                        failReason = OTA_ERROR_STRING;
                        Serial.printf("[OTA] Update.end failed: %s\n", failReason.c_str());
                    }
                }
                #undef OTA_ERROR_STRING
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
// doSetMqtt：在线更新 MQTT 连接配置
//   流程：断开当前 MQTT → 临时客户端测试新连接 → 成功则保存并切换，失败则恢复原连接
//   注意：测试客户端与主客户端顺序使用，避免双 TLS 上下文同时占用内存（ESP8266 关键）
// ─────────────────────────────────────────────────────────────────────────────
void doSetMqtt(const char* newServer, uint16_t newPort,
               const char* newUser,   const char* newPass, const char* newId) {
    // 1. 用当前配置作基础，仅覆盖非空字段
    char   testServer[64];  strlcpy(testServer, strlen(newServer) > 0 ? newServer : cfg.mqtt_server, sizeof(testServer));
    uint16_t testPort =     newPort > 0       ? newPort   : cfg.mqtt_port;
    char   testUser[64];    strlcpy(testUser,   strlen(newUser)   > 0 ? newUser   : cfg.mqtt_user,   sizeof(testUser));
    char   testPass[64];    strlcpy(testPass,   strlen(newPass)   > 0 ? newPass   : cfg.mqtt_pass,   sizeof(testPass));
    char   testId[32];      strlcpy(testId,     strlen(newId)     > 0 ? newId     : cfg.mqtt_id,     sizeof(testId));

    Serial.printf("[SET_MQTT] testing → %s:%u  user=%s  id=%s\n",
        testServer, testPort, testUser, testId);

    // 2. 断开当前 MQTT（释放 TLS 资源）
    // 失败时直接用 cfg.* 回退（cfg 在失败分支中未被修改）
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
        ok = testMqtt.connect(testId, testUser, testPass);
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
        // 4a. 写入新配置并持久化
        strlcpy(cfg.mqtt_server, testServer, sizeof(cfg.mqtt_server));
        cfg.mqtt_port = testPort;
        strlcpy(cfg.mqtt_user, testUser, sizeof(cfg.mqtt_user));
        strlcpy(cfg.mqtt_pass, testPass, sizeof(cfg.mqtt_pass));
        strlcpy(cfg.mqtt_id,   testId,   sizeof(cfg.mqtt_id));

        // 更新订阅/发布主题（mqtt_id 可能已变）
        snprintf(TOPIC_SUB, sizeof(TOPIC_SUB), "home/wol/%s/cmd",   cfg.mqtt_id);
        snprintf(TOPIC_PUB, sizeof(TOPIC_PUB), "home/wol/%s/event", cfg.mqtt_id);

        saveConfig();
        Serial.println(F("[SET_MQTT] config saved, reconnecting to new broker"));

        mqtt.setServer(cfg.mqtt_server, cfg.mqtt_port);
        connectMQTT();
        pubEvent("ok:mqtt_updated");

    } else {
        // 4b. cfg 未变，直接用原配置重连
        Serial.println(F("[SET_MQTT] reverting to original broker"));
        mqtt.setServer(cfg.mqtt_server, cfg.mqtt_port);

        // cfg 未变，connectMQTT 会用 cfg 里的凭据
        connectMQTT();

        char buf[128];
        snprintf(buf, sizeof(buf),
            "{\"event\":\"error:mqtt_connect_failed\",\"rc\":%d,\"uptime\":%lu,\"heap\":%u}",
            testRc, millis() / 1000, ESP.getFreeHeap());
        mqtt.publish(TOPIC_PUB, buf);
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
        sendMagicPacket(target_mac);
        StaticJsonDocument<96> resp;
        resp["event"] = "wol";
        resp["mac"]   = target_mac;
        pubJson(resp);

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
        const char* newId     = doc["id"]     | "";

        // 至少要有一个字段发生实质性变化
        bool anyChange = (strlen(newServer) > 0 && strcmp(newServer, cfg.mqtt_server) != 0)
                      || (newPort > 0            && newPort != cfg.mqtt_port)
                      || (strlen(newUser) > 0    && strcmp(newUser, cfg.mqtt_user) != 0)
                      || (strlen(newPass) > 0    && strcmp(newPass, cfg.mqtt_pass) != 0)
                      || (strlen(newId)   > 0    && strcmp(newId,   cfg.mqtt_id)   != 0);
        if (!anyChange) {
            pubEvent("error:set_mqtt_no_change");
        } else {
            doSetMqtt(newServer, newPort, newUser, newPass, newId);
        }

    // ── ota ──
    } else if (strcmp(cmd, "ota") == 0) {
        const char* url = doc["url"] | "";
        if (strlen(url) == 0) {
            pubEvent("error:ota_url_missing");
        } else {
            doOTA(url);   // 内部处理所有结果上报，成功时直接重启
        }

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
// ESP32-C3 先扫描，优先尝试当前可见网络；ESP8266 按列表顺序盲试。
// 全部失败时：首次连接进配置模式，曾经连上过则重启（SDK 下次启动自动重试）。
void connectWiFiMulti() {
    if (wifiCount == 0) {
        Serial.println(F("[WiFi] no networks in history → config mode"));
        enterConfigMode();  // 永不返回
    }

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);  // 轮试期间手动管理，连上后再开启
#ifdef ESP8266
    if (cfg.wifi_tx_power > 0)
        WiFi.setOutputPower((float)cfg.wifi_tx_power);
#else
    {
        int8_t txp = cfg.wifi_tx_power;
#ifdef CONFIG_IDF_TARGET_ESP32C3
        if (txp == 0) txp = 15;
#endif
        if (txp > 0) esp_wifi_set_max_tx_power((int8_t)(txp * 4));
    }
#endif
    Serial.printf("[WiFi] TX power = %ddBm%s\n", cfg.wifi_tx_power,
        (cfg.wifi_tx_power == 0) ? " (platform default)" : "");

    // ── 构建候选顺序（LRU 位置索引） ─────────────────────────────────────────
    uint8_t order[WIFI_MAX_NETWORKS];
    uint8_t orderCount = 0;

#if !defined(ESP8266)
    // ESP32-C3：先扫描，可见网络排前面（隐藏网络仍在列表末尾兜底）
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
#else
    // ESP8266：按列表顺序盲试
    for (uint8_t i = 0; i < wifiCount; i++) order[orderCount++] = i;
#endif

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
        Serial.println(F("[WiFi] first run → config mode"));
        enterConfigMode();  // 永不返回
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
#if defined(ESP8266)
    Serial.println(F("\n\n====== WoL ESP-12F boot ======"));
#else
    Serial.println(F("\n\n====== WoL ESP32-C3 boot ======"));
#endif
    Serial.println(F("Firmware: " FW_VERSION));

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, HIGH);  // 关灯（active low）
    pinMode(CFG_RESET_PIN, INPUT_PULLUP);

    // ── 1. 挂载文件系统 ──
#if defined(ESP8266)
    if (!LittleFS.begin()) {
        Serial.println(F("[FS] format..."));
        LittleFS.format();
        LittleFS.begin();
    }
#else
    LittleFS.begin(true);  // formatOnFail
#endif
    Serial.println(F("[FS] mounted"));

    // ── 2. 加载配置（需先于所有模式检测，确保 cfg 已填充） ──
    bool configOk = loadConfig();

    // ── 2b. 加载 WiFi 历史列表（依赖 cfg.wifi_ssid 做迁移，必须在 loadConfig 之后）──
    bool hasWifi = loadWifiNetworks();

    // ── 3. 重配置标志检测 → 串口 CLI ──
    checkReconfigFlag();

    // ── 4. SoftAP 标志检测 → SoftAP 网页配置 ──
    checkSoftAPFlag();

    // ── 5. 配置校验：无有效配置 → 串口 CLI ──
    if (!configOk || !hasWifi) {
        Serial.println(F("[CFG] no valid config → config mode"));
        enterConfigMode();  // 永不返回
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
    // 在 ESP8266 上 configTime() 会分配 SNTP 内部结构体，约占用 1~2KB 堆，
    // 当前不需要，待引入绝对时间戳功能时再启用。
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

    // ── 7. LED 非阻塞闪烁驱动 ──
    blinkTick();
}
