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
 *   {"cmd":"info"}                             查询设备信息
 *   {"cmd":"config"}                           查询所有配置
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
  #include <ESP8266httpUpdate.h>
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
    StaticJsonDocument<512> doc;
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
    StaticJsonDocument<512> doc;
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

    // 必填项校验
    return strlen(cfg.wifi_ssid)   > 0
        && strlen(cfg.mqtt_server) > 0
        && strlen(cfg.mqtt_id)     > 0
        && strlen(cfg.wol_mac)     == 12;
}

// ─────────────────────────────────────────────────────────────────────────────
// writeFlag：在 LittleFS 写入空标志文件（用于跨重启传递模式信号）
// ─────────────────────────────────────────────────────────────────────────────
void writeFlag(const char* path) {
    File f = LittleFS.open(path, "w");
    if (f) f.close();
}

// ─────────────────────────────────────────────────────────────────────────────
// enterConfigMode：串口配置 CLI，保存后重启，永不正常返回
// ─────────────────────────────────────────────────────────────────────────────
static void printConfigHelp() {
    Serial.println(F("\n====== CONFIG MODE ======"));
    Serial.println(F("Firmware: " FW_VERSION));
    Serial.println(F("Commands:"));
    Serial.println(F("  set wifi_ssid   <value>"));
    Serial.println(F("  set wifi_pass   <value>"));
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
                Serial.println(F("[CFG] staged config:"));
                Serial.printf("  wifi_ssid  : %s\n",  staged.wifi_ssid);
                Serial.printf("  wifi_pass  : %s\n",  strlen(staged.wifi_pass) ? staged.wifi_pass : "(not set)");
                Serial.printf("  mqtt_server: %s\n",  staged.mqtt_server);
                Serial.printf("  mqtt_port  : %u\n",  staged.mqtt_port);
                Serial.printf("  mqtt_user  : %s\n",  staged.mqtt_user);
                Serial.printf("  mqtt_pass  : %s\n",  strlen(staged.mqtt_pass) ? staged.mqtt_pass : "(not set)");
                Serial.printf("  mqtt_id    : %s\n",  staged.mqtt_id);
                Serial.printf("  wol_mac    : %s\n",  staged.wol_mac);
                Serial.printf("  ap_full_config: %s\n", staged.ap_full_config ? "true" : "false");
                Serial.printf("  wifi_tx_power : %ddBm (0=platform default)\n", staged.wifi_tx_power);

            } else if (line == "save") {
                bool ok = true;
                if (strlen(staged.wifi_ssid)   == 0) { Serial.println(F("[CFG] error: wifi_ssid required"));   ok = false; }
                if (strlen(staged.mqtt_server) == 0) { Serial.println(F("[CFG] error: mqtt_server required")); ok = false; }
                if (strlen(staged.mqtt_id)     == 0) { Serial.println(F("[CFG] error: mqtt_id required"));     ok = false; }
                { bool hexOk = (strlen(staged.wol_mac) == 12);
                  for (int i = 0; hexOk && i < 12; i++) hexOk = isxdigit((unsigned char)staged.wol_mac[i]);
                  if (!hexOk) { Serial.println(F("[CFG] error: wol_mac must be 12 hex chars")); ok = false; } }
                if (ok) {
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
                Serial.println(F("[CFG] config erased, rebooting..."));
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
                    if      (key == "wifi_ssid")   strlcpy(staged.wifi_ssid,   val.c_str(), sizeof(staged.wifi_ssid));
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
static String buildAPPage(const String& statusHtml) {
    String html = FPSTR(CONFIG_HTML);
    html.replace("%STATUS%",  statusHtml);
    html.replace("%SSID%",    String(cfg.wifi_ssid));
    html.replace("%WPASS%",   String(cfg.wifi_pass));
    html.replace("%WOLMAC%",  String(cfg.wol_mac));
    html.replace("%VERSION%", FW_VERSION);
    if (cfg.ap_full_config) {
        String mqtt = FPSTR(MQTT_FIELDS_HTML);
        mqtt.replace("%MQTTSVR%",  String(cfg.mqtt_server));
        mqtt.replace("%MQTTPORT%", String(cfg.mqtt_port));
        mqtt.replace("%MQTTUSER%", String(cfg.mqtt_user));
        mqtt.replace("%MQTTPASS%", String(cfg.mqtt_pass));
        mqtt.replace("%MQTTID%",   String(cfg.mqtt_id));
        html.replace("%FULLBLOCK%", mqtt);
    } else {
        html.replace("%FULLBLOCK%", "");
    }
    return html;
}

static void handleAPScan() {
    int n = WiFi.scanNetworks();
    String json = "[";
    for (int i = 0; i < n; i++) {
        if (i > 0) json += ",";
        String ssid = WiFi.SSID(i);
        ssid.replace("\\", "\\\\");
        ssid.replace("\"", "\\\"");
        json += "\"" + ssid + "\"";
    }
    json += "]";
    WiFi.scanDelete();
    apServer.send(200, "application/json", json);
}

static void handleAPRoot() {
    apServer.send(200, "text/html", buildAPPage(""));
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
        String msg = "<div class=\"msg err\">" + err + "</div>";
        apServer.send(400, "text/html", buildAPPage(msg));
        return;
    }

    // 写入 cfg
    strlcpy(cfg.wifi_ssid, ssid.c_str(),   sizeof(cfg.wifi_ssid));
    strlcpy(cfg.wifi_pass, wpass.c_str(),  sizeof(cfg.wifi_pass));
    strlcpy(cfg.wol_mac,   wolmac.c_str(), sizeof(cfg.wol_mac));
    cfg.wifi_ever_ok = false;

    if (cfg.ap_full_config) {
        strlcpy(cfg.mqtt_server, mqttsvr.c_str(), sizeof(cfg.mqtt_server));
        int port = apServer.arg("mqttport").toInt();
        cfg.mqtt_port = (port > 0 && port <= 65535) ? (uint16_t)port : 8883;
        strlcpy(cfg.mqtt_user, apServer.arg("mqttuser").c_str(), sizeof(cfg.mqtt_user));
        strlcpy(cfg.mqtt_pass, apServer.arg("mqttpass").c_str(), sizeof(cfg.mqtt_pass));
        strlcpy(cfg.mqtt_id,   mqttid.c_str(),  sizeof(cfg.mqtt_id));
    }

    saveConfig();
    Serial.println(F("[AP] config saved, rebooting..."));

    apServer.send(200, "text/html",
        F("<!DOCTYPE html><html><head><meta charset='UTF-8'>"
          "<meta name='viewport' content='width=device-width,initial-scale=1'>"
          "<style>body{font-family:system-ui,sans-serif;display:flex;align-items:center;"
          "justify-content:center;min-height:100vh;background:#f1f5f9;margin:0}"
          ".card{background:#fff;border-radius:12px;box-shadow:0 4px 24px rgba(0,0,0,.09);"
          "padding:32px 36px;text-align:center;max-width:320px}"
          "h2{color:#16a34a;margin-bottom:8px}p{color:#64748b;font-size:.9rem}</style></head>"
          "<body><div class='card'><h2>✓ 保存成功</h2>"
          "<p>配置已写入，设备正在重启，请稍候...</p></div></body></html>"));
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

    apServer.on("/",      HTTP_GET,  handleAPRoot);
    apServer.on("/scan",  HTTP_GET,  handleAPScan);
    apServer.on("/save",  HTTP_POST, handleAPSave);
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
//   重定向：ESP8266 由 ESPhttpUpdate 自动跟踪；ESP32-C3 由 HTTPClient 跟踪
//   TLS：setInsecure()，与主连接一致，不验证证书
// ─────────────────────────────────────────────────────────────────────────────
void doOTA(const char* urlParam) {
    // 立即复制 URL，避免后续操作污染调用方的栈帧或 MQTT 缓冲区
    char url[512];
    strlcpy(url, urlParam, sizeof(url));

    Serial.printf("[OTA] starting: %s\n", url);

    // 1. 发布 ota_start（此时 MQTT 仍连接）
    {
        char buf[320];
        snprintf(buf, sizeof(buf),
            "{\"event\":\"ota_start\",\"url\":\"%s\",\"uptime\":%lu,\"heap\":%u}",
            url, millis() / 1000, ESP.getFreeHeap());
        mqtt.publish(TOPIC_PUB, buf);
    }
    delay(300);  // 等待 MQTT 发送完成

    // 2. 断开 MQTT，释放 TLS 上下文内存（ESP8266 关键步骤）
    mqtt.disconnect();
    delay(100);

    bool   success = false;
    String failReason;

#ifdef ESP8266
    // ── ESP8266：使用 ESPhttpUpdate，自动跟踪重定向 ──
    {
        WiFiClientSecure otaClient;
        otaClient.setInsecure();
        ESPhttpUpdate.rebootOnUpdate(false);
        ESPhttpUpdate.onProgress([](int cur, int total) {
            if (total > 0)
                Serial.printf("[OTA] %d%% (%d/%d)\n", cur * 100 / total, cur, total);
        });
        t_httpUpdate_return ret = ESPhttpUpdate.update(otaClient, url);
        if (ret == HTTP_UPDATE_OK) {
            success = true;
        } else {
            failReason = ESPhttpUpdate.getLastErrorString();
            Serial.printf("[OTA] failed: %s\n", failReason.c_str());
        }
    }
#else
    // ── ESP32-C3：使用 HTTPClient + Update，setFollowRedirects 跟踪重定向 ──
    {
        WiFiClientSecure otaClient;
        otaClient.setInsecure();

        HTTPClient http;
        http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
        http.setTimeout(30000);

        if (!http.begin(otaClient, url)) {
            failReason = F("http.begin failed");
        } else {
            int code = http.GET();
            Serial.printf("[OTA] HTTP %d\n", code);
            if (code == HTTP_CODE_OK) {
                int totalSize = http.getSize();
                Serial.printf("[OTA] size: %d bytes\n", totalSize);
                if (!Update.begin(totalSize > 0 ? totalSize : UPDATE_SIZE_UNKNOWN)) {
                    failReason = Update.errorString();
                } else {
                    Update.onProgress([](size_t done, size_t total) {
                        if (total > 0 && done % (total / 10 + 1) == 0)
                            Serial.printf("[OTA] %u%%\n", (unsigned)(done * 100 / total));
                    });
                    WiFiClient* stream = http.getStreamPtr();
                    stream->setTimeout(10000);  // 每次 chunk 读取最长等 10s
                    size_t written = Update.writeStream(*stream);
                    Serial.printf("[OTA] written %u bytes\n", (unsigned)written);
                    if (Update.end(true) && Update.isFinished()) {
                        success = true;
                    } else {
                        failReason = Update.errorString();
                    }
                }
            } else {
                failReason = String(F("HTTP ")) + code;
            }
            http.end();
        }
    }
#endif

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
        char buf[256];
        snprintf(buf, sizeof(buf),
            "{\"event\":\"ota_fail\",\"reason\":\"%s\",\"uptime\":%lu,\"heap\":%u}",
            failReason.c_str(), millis() / 1000, ESP.getFreeHeap());
        mqtt.publish(TOPIC_PUB, buf);
    }
    delay(500);
    ESP.restart();
}

// ─────────────────────────────────────────────────────────────────────────────
// mqttCallback：处理下行指令
// ─────────────────────────────────────────────────────────────────────────────
void mqttCallback(char* topic, byte* payload, unsigned int len) {
    char msg[512] = {};
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
        resp["mac"]        = cfg.wol_mac;
        resp["ssid"]       = cfg.wifi_ssid;
        resp["reconnects"] = mqttReconnectCount;
        pubJson(resp);

    // ── config ──
    } else if (strcmp(cmd, "config") == 0) {
        StaticJsonDocument<384> resp;
        resp["event"]            = "config";
        resp["wifi_ssid"]        = cfg.wifi_ssid;
        resp["mqtt_server"]      = cfg.mqtt_server;
        resp["mqtt_port"]        = cfg.mqtt_port;
        resp["mqtt_user"]        = cfg.mqtt_user;
        resp["mqtt_id"]          = cfg.mqtt_id;
        resp["wol_mac"]          = cfg.wol_mac;
        resp["status_interval"]  = statusIntervalMs / 1000;
        pubJson(resp);

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

// startWiFi：发起连接，只在 setup() 调用一次
// setAutoReconnect(true) 确保后续断线由 SDK 自动重连，无需重复 begin()
void startWiFi() {
    Serial.printf("[WiFi] connecting to %s\n", cfg.wifi_ssid);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
#ifdef ESP8266
    if (cfg.wifi_tx_power > 0)
        WiFi.setOutputPower((float)cfg.wifi_tx_power);   // ESP8266：0-20.5 dBm
#else
    {
        int8_t txp = cfg.wifi_tx_power;
#ifdef CONFIG_IDF_TARGET_ESP32C3
        if (txp == 0) txp = 15;  // ESP32-C3 天线缺陷+LDO不足，默认限制 15dBm
#endif
        if (txp > 0) esp_wifi_set_max_tx_power((int8_t)(txp * 4));
    }
#endif
    Serial.printf("[WiFi] TX power = %ddBm%s\n", cfg.wifi_tx_power,
        (cfg.wifi_tx_power == 0) ? " (platform default)" : "");

    // 首次连接（或重新配置后）先主动扫描，快速检测 SSID 是否存在，避免等待整个超时周期。
    // 仅在 !wifi_ever_ok 时执行：wifi_ever_ok=true 说明曾经连上过，SSID 找不到可能是路由器
    // 暂时离线，此时不应快速失败。注意：隐藏网络不会出现在扫描结果中，会绕过此检测。
#if !defined(ESP8266)
    if (!cfg.wifi_ever_ok) {
        Serial.println(F("[WiFi] scanning for SSID..."));
        int n = WiFi.scanNetworks();
        bool found = false;
        for (int i = 0; i < n; i++) {
            if (WiFi.SSID(i) == cfg.wifi_ssid) { found = true; break; }
        }
        WiFi.scanDelete();
        if (!found) {
            Serial.printf("[WiFi] SSID \"%s\" not found → config mode\n", cfg.wifi_ssid);
            enterConfigMode();  // 永不返回
        }
    }
#endif

    WiFi.begin(cfg.wifi_ssid, cfg.wifi_pass);
}

// waitWiFiConnected：等待连接完成，setup() 和 loop() 均可调用
// wifi_ever_ok 决定超时策略：
//   首次连接（false）→ 超时后进配置模式，方便用户重新填写凭据
//   曾经连上过（true）→ 超时后重启，避免路由器临时宕机时频繁进配置模式
void waitWiFiConnected() {
    if (WiFi.status() == WL_CONNECTED) return;

    unsigned long timeout = cfg.wifi_ever_ok ? WIFI_TIMEOUT_MS : WIFI_TIMEOUT_FIRSTRUN_MS;
    unsigned long t = millis();

    // 循环只负责等待，连接过程交给 SDK 处理，不手动干预
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
        // WL_WRONG_PASSWORD(7, ESP8266) / WL_CONNECT_FAILED(4) / WL_NO_SSID_AVAIL(1) 是明确的
        // 失败状态，仅在首次连接时才快速进配置模式；曾经连上过的情况下这些状态可能是暂时的
        // （路由器重启、信号抖动），交给超时逻辑决定走重启还是配置模式
        if (!cfg.wifi_ever_ok && (s == WL_CONNECT_FAILED || s == WL_NO_SSID_AVAIL || s == 7)) {
            Serial.printf("\n[WiFi] connect failed (%s) → config mode\n", wifiStatusName(s));
            enterConfigMode();  // 永不返回
        }
        checkRuntimeTriggers();
        if (millis() - t > timeout) {
            Serial.println();
            if (!cfg.wifi_ever_ok) {
                Serial.println(F("[WiFi] first-run timeout → config mode"));
                enterConfigMode();  // 永不返回
            } else {
                Serial.println(F("[WiFi] timeout → restart"));
                ESP.restart();
            }
        }
    }
    wifiConnectMs = millis() - t;
    Serial.printf("\n[WiFi] connected in %lums, IP=%s\n", wifiConnectMs, WiFi.localIP().toString().c_str());

    // 首次连接成功后持久化标志，后续超时走重启而非进配置模式
    if (!cfg.wifi_ever_ok) {
        cfg.wifi_ever_ok = true;
        if (saveConfig()) {
            Serial.println(F("[CFG] wifi_ever_ok saved"));
        } else {
            Serial.println(F("[CFG] WARNING: wifi_ever_ok save failed"));
        }
    }
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

    // ── 3. 重配置标志检测 → 串口 CLI ──
    checkReconfigFlag();

    // ── 4. SoftAP 标志检测 → SoftAP 网页配置 ──
    checkSoftAPFlag();

    // ── 5. 配置校验：无有效配置 → 串口 CLI ──
    if (!configOk) {
        Serial.println(F("[CFG] no valid config → config mode"));
        enterConfigMode();  // 永不返回
    }

    // ── 6. 填充运行时主题 ──
    snprintf(TOPIC_SUB, sizeof(TOPIC_SUB), "home/wol/%s/cmd",   cfg.mqtt_id);
    snprintf(TOPIC_PUB, sizeof(TOPIC_PUB), "home/wol/%s/event", cfg.mqtt_id);

    // ── 7. 打印启动信息 ──
    Serial.println(F("------- 当前配置 ---------------"));
    Serial.printf("  固件版本   : %s\n",  FW_VERSION);
    Serial.println(F("  [WiFi]"));
    Serial.printf("    SSID     : %s\n",  cfg.wifi_ssid);
    Serial.printf("    密码     : %s\n",  cfg.wifi_pass);
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

    // ── 8. 连接 WiFi ──
    startWiFi();
    waitWiFiConnected();

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
    mqtt.setBufferSize(768);

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
