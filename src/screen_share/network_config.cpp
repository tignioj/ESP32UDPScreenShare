#include "network_config.h"
#include <WebServer.h>
#include <Preferences.h>
#include <DNSServer.h>
/*
配网流程
启动
 │
 ├─ 读取 Flash 中 WiFi
 │     ├─ 成功 → 正常工作
 │     └─ 失败
 │
 └─ 开启 AP（ESP32ScreenShareUDP）
       手机连接
       ↓
       自动弹出网页
       ↓
       输入 WiFi
       ↓
       保存 → 重启 → 连接成功

*/
#define AP_SSID "ESP32ScreenShareUDP"
#define AP_CHANNEL 1

static WebServer server(80);
static DNSServer dns;
static Preferences prefs;

static String saved_ssid;
static String saved_pass;

// ================= HTML =================
static const char* WIFI_HTML = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32 WiFi Config</title>
</head>
<body>
<h2>ESP32 WiFi Configurator</h2>
<form action="/save" method="POST">
2.4G SSID:<br><input name="s" placeholder="2.4G WiFi only"><br>
Password:<br><input name="p" type="password"><br><br>
<input type="submit" value="Save">
</form>
</body>
</html>
)rawliteral";

// ================= 保存配置 =================
static void handleSave() {
    saved_ssid = server.arg("s");
    saved_pass = server.arg("p");

    prefs.putString("ssid", saved_ssid);
    prefs.putString("pass", saved_pass);

    server.send(200, "text/html",
        "<h2>Saved. Rebooting...</h2>");

    delay(1000);
    ESP.restart();
}

// ================= Portal =================
static void startPortal() {
    Serial.println("[WiFi] Starting AP Portal");

    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, nullptr, AP_CHANNEL);

    dns.start(53, "*", WiFi.softAPIP());

    server.on("/", []() {
        server.send(200, "text/html", WIFI_HTML);
    });

    // captive portal 兼容
    server.on("/generate_204", []() { server.send(200, "text/html", WIFI_HTML); });
    server.on("/fwlink", []() { server.send(200, "text/html", WIFI_HTML); });
    server.on("/hotspot-detect.html", []() { server.send(200, "text/html", WIFI_HTML); });
    server.on("/save", HTTP_POST, handleSave);

    server.begin();
}

// ================= 尝试连接 =================
static bool connectSTA() {
    if (saved_ssid.isEmpty()) return false;

    WiFi.mode(WIFI_STA);
    WiFi.begin(saved_ssid.c_str(), saved_pass.c_str());

    Serial.printf("[WiFi] Connecting to %s\n", saved_ssid.c_str());

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        delay(300);
        if (millis() - start > 8000) {
            Serial.println("[WiFi] Connect timeout");
            return false;
        }
    }

    Serial.println("[WiFi] Connected");
    return true;
}

// ================= API =================
namespace NetworkConfig {

void begin() {
    prefs.begin("wifi", false);
    saved_ssid = prefs.getString("ssid", "");
    saved_pass = prefs.getString("pass", "");

    if (!connectSTA()) {
        startPortal();
        while (true) {
            dns.processNextRequest();
            server.handleClient();
            delay(2);
        }
    }
}

bool isConnected() {
    return WiFi.status() == WL_CONNECTED;
}

IPAddress localIP() {
    return WiFi.localIP();
}

}
