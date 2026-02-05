#include <WiFi.h>
#include <WiFiUdp.h>
#include "common.h"
#include "scale_function2.h"
#include "network_config.h"
// 本代码是screen share一种实验：把绘制线程放入了core1的xTask,而udp线程放进loop，画面撕裂感大幅度下降，吞吐率1500-1600pac/s
// ================= WiFi =================
const char* ssid = WIFI_SSID_STR;
const char* password = WIFI_PASSWORD_STR;

WiFiUDP udp;
#define UDP_PORT 8888

// ================= Image =================
#define IMG_W 240
#define RGB_LINE_BATCH 8  // 需要足够大，因为放大后行数可能增加

// ================= Frame Buffer =================
#define FRAME_BUF_COUNT 12 // 214492

enum BufState {
    BUF_FREE,
    BUF_FILLING,
    BUF_READY,
    BUF_DISPLAYING
};

struct FrameData {
    uint16_t frame_id;// = (header[0] << 8) | header[1];
    uint16_t y_start;
    uint16_t line_count;
    uint16_t lines[IMG_W * RGB_LINE_BATCH];
    volatile BufState state;
};

FrameData frameBuf[FRAME_BUF_COUNT];

// ================= DMA =================
uint16_t* dmaBuf[2];
volatile uint8_t dmaSel = 0;

// ================= Stats =================
volatile uint32_t frameCount = 0;
volatile uint32_t dropCount = 0;
volatile uint32_t udpPackets = 0;

// ================= UDP Receiver Function =================
IRAM_ATTR  bool processUDPPacket() {
    static uint8_t rxBuf[1460];
    
    int packetSize = udp.parsePacket();
    if (packetSize <= 0) {
        return false;
    }
    udpPackets++;

    // ------------------ 读 Header ------------------
    uint8_t header[5];
    if (udp.read(header, 5) != 5) {
        udp.flush();
        return true; // 处理了一个包但失败了
    }

    uint16_t frame_id = (header[0] << 8) | header[1];
    uint16_t src_y0 = (header[2] << 8) | header[3];
    uint8_t flags = header[4];

    uint8_t resolution = (flags >> 6) & 0x03; // 0=240,1=180,2=120
    uint8_t color_mode = (flags >> 4) & 0x03; // 0=RGB565,1=RGB332
    uint8_t src_lines = flags & 0x0F;

    if (src_lines == 0 || src_lines > RGB_LINE_BATCH) {
        udp.flush();
        return true;
    }

    bool is_rgb565 = (color_mode == 0);

    // ------------------ 源尺寸 ------------------
    int src_w, src_h;
    switch (resolution) {
        case 0: src_w = src_h = 240; break;
        case 1: src_w = src_h = 180; break;
        case 2: src_w = src_h = 120; break;
        default:
            udp.flush();
            return true;
    }

    // ------------------ 计算接收大小 ------------------
    uint32_t bytes_per_px = is_rgb565 ? 2 : 1;
    uint32_t expect = src_w * src_lines * bytes_per_px;

    // ------------------ 找空 buffer ------------------
    FrameData* f = nullptr;
    for (int i = 0; i < FRAME_BUF_COUNT; i++) {
        if (frameBuf[i].state == BUF_FREE) {
            f = &frameBuf[i];
            f->state = BUF_FILLING;
            break;
        }
    }

    if (!f) {
        dropCount++;
        udp.flush();
        return true;
    }

    if (udp.read(rxBuf, expect) != expect) {
        f->state = BUF_FREE;
        return true;
    }

    // =================================================
    //            分辨率统一 → 240 RGB565
    // =================================================
    uint16_t* dst = f->lines;
    int dst_y0 = 0;
    int dst_lines = 0;

    // =============== 240 → 240 ======================
    if (src_w == 240) {
        dst_y0 = src_y0;
        dst_lines = src_lines;
        
        if (is_rgb565) {
            memcpy(dst, rxBuf, 240 * src_lines * 2);
        } else {
            int n = src_w * src_lines;
            uint16_t* d = dst;
            uint8_t* src = rxBuf;
            while (n >= 4) {
                d[0] = rgb332_to_565_lut[src[0]];
                d[1] = rgb332_to_565_lut[src[1]];
                d[2] = rgb332_to_565_lut[src[2]];
                d[3] = rgb332_to_565_lut[src[3]];
                src += 4;
                d   += 4;
                n   -= 4;
            }   
            while (n--) {
                *d++ = rgb332_to_565_lut[*src++];
            }
        }
    }
    // =============== 180 → 240 ======================
    else if (src_w == 180) {
        // 计算目标行范围
        dst_y0 = (src_y0 * 240 + 120) / 180;  // 四舍五入
        dst_lines = (src_lines * 240 + 179) / 180; // 向上取整
        // 检查缓冲区是否足够
        if (dst_lines > RGB_LINE_BATCH) {
            f->state = BUF_FREE;
            udp.flush();
            return true;
        }
        if (is_rgb565) {
            scale_180_to_240_rgb565(
                (uint16_t*)rxBuf,
                dst,
                src_lines
            );
        } else {
            scale_180_to_240_rgb332(
                rxBuf,
                dst,
                src_lines
            );
        }
    }
    // =============== 120 → 240 ======================
    else if (src_w == 120) {
        // 计算目标行范围
        dst_y0 = src_y0 * 2;
        dst_lines = src_lines * 2;
        // 确保不超出240边界
        if (dst_y0 + dst_lines > 240) {
            dst_lines = 240 - dst_y0;
        }

        // 检查缓冲区是否足够
        if (dst_lines > RGB_LINE_BATCH) {
            // 如果超出，调整接收到的行数
            int max_src_lines = RGB_LINE_BATCH / 2;
            if (src_lines > max_src_lines) {
                src_lines = max_src_lines;
                dst_lines = max_src_lines * 2;
            }
        }

        if (is_rgb565) {
            scale_120_to_240_rgb565((uint16_t*)rxBuf,
            dst,
            src_lines);
        } else {
            scale_120_to_240_rgb332(
                (uint8_t*)rxBuf,
                dst,
                src_lines);
        }
    }

    // ------------------ 提交 ------------------
    f->frame_id = frame_id;
    f->y_start = dst_y0;
    f->line_count = dst_lines;
    f->state = BUF_READY;
    
    return true;
}

// ================= Draw Task =================
IRAM_ATTR void drawTask(void* param) {
    while (1) {
        FrameData* f = nullptr;

        // 找 READY 的缓冲区
        for (int i = 0; i < FRAME_BUF_COUNT; i++) {
            if (frameBuf[i].state == BUF_READY) {
                f = &frameBuf[i];
                break;
            }
        }
        
        if (!f) {
            vTaskDelay(1); // 没有数据时短暂延时
            continue;
        }

        f->state = BUF_DISPLAYING;

        uint8_t nextDma = dmaSel ^ 1;

        // 等待 DMA 完成
        tft->dmaWait();

        memcpy(
            dmaBuf[nextDma],
            f->lines,
            IMG_W * f->line_count * 2
        );

        tft->startWrite();
        tft->pushImageDMA(
            0,
            f->y_start,
            IMG_W,
            f->line_count,
            dmaBuf[nextDma]
        );
        tft->endWrite();

        dmaSel = nextDma;
        f->state = BUF_FREE;
        frameCount++;
        
        // 如果需要，可以在这里添加小的延时来控制绘制频率
        // vTaskDelay(1);
    }
}

// ================= Setup =================
void setup() {
    Serial.begin(115200);
    tft_init();
    setCpuFrequencyMhz(240);
    
    tft->initDMA();
    tft->setSwapBytes(true);

    tft->setTextFont(1);
    tft->fillScreen(TFT_BLACK);
    tft->setTextSize(2);
    tft->setTextColor(TFT_WHITE);
    tft->println("No network! Please connect to AP:ESP32ScreenShareUDP, then input you 2.4G WiFi and password.");
    tft->setCursor(0, 10);
    
    NetworkConfig::begin();   // ⭐ 一行解决 WiFi / AP / 配网
    Serial.println("\nWiFi connected");

    udp.begin(UDP_PORT);

    for (int i = 0; i < FRAME_BUF_COUNT; i++) {
        frameBuf[i].state = BUF_FREE;
    }
    init_scale_maps();

    // 分配 DMA 缓冲区
    dmaBuf[0] = (uint16_t*)heap_caps_malloc(
        IMG_W * RGB_LINE_BATCH * 2,
        MALLOC_CAP_DMA
    );
    dmaBuf[1] = (uint16_t*)heap_caps_malloc(
        IMG_W * RGB_LINE_BATCH * 2,
        MALLOC_CAP_DMA
    );

    if (!dmaBuf[0] || !dmaBuf[1]) {
        Serial.println("DMA alloc failed");
        while (1);
    }

    // 创建绘制任务（运行在核心0或1，根据需求调整）
    xTaskCreatePinnedToCore(
        drawTask,
        "draw_task",
        4096,  // 绘制任务不需要太大栈空间
        nullptr,
        2,     // 优先级可以调整
        nullptr,
        1      // 核心1
    );
    
    tft->fillScreen(TFT_BLACK);
    String wifi_str = WiFi.localIP().toString() + ":8888";
    tft->println(wifi_str);
    tft->setCursor(0, 34);
    tft->setTextFont(2);
    tft->setTextSize(1);
    tft->println("ScreenShareUDP v0.0.2, UDP in loop, draw in task.");
    tft->setCursor(0, 74);
    tft->println("Client: https://github.com/tignioj/ESP32UDPScreenShareClient");
}

// ================= Debug Info =================
void printDebugInfo() {
    static uint32_t lastPrint = 0;
    uint32_t now = millis();
    
    if (now - lastPrint > 5000) {
        lastPrint = now;
        
        Serial.printf("内存: %u, ", ESP.getFreeHeap());
        Serial.printf("UDP包/5秒: %u, ", udpPackets);
        Serial.printf("丢包数: %u, ", dropCount);
        Serial.printf("显示帧数: %u, ", frameCount);
        
        // 显示缓冲区状态
        Serial.print("缓冲区状态: ");
        for (int i = 0; i < FRAME_BUF_COUNT; i++) {
            switch(frameBuf[i].state) {
                case BUF_FREE: Serial.print("F"); break;
                case BUF_FILLING: Serial.print("I"); break;
                case BUF_READY: Serial.print("R"); break;
                case BUF_DISPLAYING: Serial.print("D"); break;
            }
        }
        Serial.println();
        
        udpPackets = 0;
        frameCount = 0;
    }
}

// ================= Loop =================
void loop() {
    // 处理所有可用的UDP包
    bool packetProcessed = true;
    while (packetProcessed) {
        packetProcessed = processUDPPacket();
    }
    
    // 显示调试信息
    // printDebugInfo();
    
    // 短暂延时，防止过度占用CPU
    // delay(1);
    delayMicroseconds(50);
}