#pragma once
#include <WiFi.h>

namespace NetworkConfig {
    void begin();
    bool isConnected();
    IPAddress localIP();
}
