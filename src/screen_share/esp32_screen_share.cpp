#include <WiFi.h>
#include <WiFiUdp.h>
#include "common.h"

// ================= WiFi =================
const char* ssid = WIFI_SSID_STR;
const char* password = WIFI_PASSWORD_STR;

WiFiUDP udp;
#define UDP_PORT 8888
// ================= Image =================
#define IMG_W 240

#define RGB_LINE_BATCH 12  // 需要足够大，因为放大后行数可能增加

// ================= Frame Buffer =================
#define FRAME_BUF_COUNT 8

enum BufState {
    BUF_FREE,
    BUF_FILLING,
    BUF_READY,
    BUF_DISPLAYING
};

struct FrameData {
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

// ================= RGB332 → RGB565 =================
static inline uint16_t rgb332_to_rgb565(uint8_t c)
{
    return ((c & 0xE0) << 8) |   // R: 3 → 5
           ((c & 0x1C) << 6) |   // G: 3 → 6
           ((c & 0x03) << 3);    // B: 2 → 5
}

// ================= 放大函数 =================

// 预计算映射表
static int scale_x_map[240];  // 水平映射表
static int scale_y_map[240];  // 垂直映射表（最大支持240行）
// 初始化映射表（在setup中调用）
void init_scale_maps() {
    // 计算水平映射
    for (int dst_x = 0; dst_x < 240; dst_x++) {
        scale_x_map[dst_x] = (dst_x * 180 + 120) / 240;  // 四舍五入
        if (scale_x_map[dst_x] >= 180) {
            scale_x_map[dst_x] = 179;
        }
    }
    
    // 计算垂直映射（最大240行）
    for (int dst_y = 0; dst_y < 240; dst_y++) {
        scale_y_map[dst_y] = (dst_y * 180 + 120) / 240;  // 四舍五入
        if (scale_y_map[dst_y] >= 180) {
            scale_y_map[dst_y] = 179;
        }
    }
}
// 最近邻插值放大 180→240 (放大系数 1.333:1)
// 使用映射表的缩放函数
static void scale_180_to_240_table(const uint8_t* src, uint16_t* dst, bool is_rgb565, int src_lines)
{
    // 计算目标行数
    int dst_lines = (src_lines * 240 + 179) / 180;
    
    for (int dst_y = 0; dst_y < dst_lines; dst_y++) {
        // 使用预计算的映射表
        int src_y = scale_y_map[dst_y];
        
        // 确保不越界
        if (src_y >= src_lines) {
            src_y = src_lines - 1;
        }
        
        for (int dst_x = 0; dst_x < 240; dst_x++) {
            int src_x = scale_x_map[dst_x];
            
            if (is_rgb565) {
                dst[dst_y * 240 + dst_x] = ((uint16_t*)src)[src_y * 180 + src_x];
            } else {
                dst[dst_y * 240 + dst_x] = rgb332_to_rgb565(src[src_y * 180 + src_x]);
            }
        }
    }
}
// 最近邻插值放大 120→240 (放大系数 2:1)
static void scale_120_to_240(const uint8_t* src, uint16_t* dst, bool is_rgb565, int src_lines)
{

    // 目标行数 = 源行数 * 2
    int dst_lines = src_lines * 2;
    // 简单复制: 每个源像素变成 2x2 个目标像素
    for (int src_y = 0; src_y < src_lines; src_y++) {
        int dst_y = src_y * 2;

        // 确保不超出目标缓冲区
        if (dst_y + 1 >= dst_lines) continue;
        for (int src_x = 0; src_x < 120; src_x++) {
            int dst_x = src_x * 2;
            
            uint16_t pixel;
            if (is_rgb565) {
                pixel = ((uint16_t*)src)[src_y * 120 + src_x];
            } else {
                pixel = rgb332_to_rgb565(src[src_y * 120 + src_x]);
            }
            
            // 填充 2x2 像素块
            dst[dst_y * 240 + dst_x] = pixel;
            dst[dst_y * 240 + dst_x + 1] = pixel;
            dst[(dst_y + 1) * 240 + dst_x] = pixel;
            dst[(dst_y + 1) * 240 + dst_x + 1] = pixel;
        }
    }
}

// ================= UDP Receiver Task =================
void udpReceiverTask(void* param)
{
    // 增大缓冲区以容纳放大后的数据
    static uint8_t rxBuf[IMG_W * RGB_LINE_BATCH * 2];

    while (1) {
        int packetSize = udp.parsePacket();
        if (packetSize <= 0) {
            vTaskDelay(1);
            continue;
        }
        udpPackets++;

        // ------------------ 读 Header ------------------
        uint8_t header[5];
        if (udp.read(header, 5) != 5) {
            udp.flush();
            continue;
        }

        uint16_t frame_id = (header[0] << 8) | header[1];
        uint16_t src_y0 = (header[2] << 8) | header[3];
        uint8_t flags = header[4];

        uint8_t resolution = (flags >> 6) & 0x03; // 0=240,1=180,2=120
        uint8_t color_mode = (flags >> 4) & 0x03; // 0=RGB565,1=RGB332
        uint8_t src_lines = flags & 0x0F;

        if (src_lines == 0 || src_lines > RGB_LINE_BATCH) {
            udp.flush();
            continue;
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
                continue;
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
            continue;
        }

        if (udp.read(rxBuf, expect) != expect) {
            f->state = BUF_FREE;
            continue;
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
                uint32_t px = 240 * src_lines;
                for (uint32_t i = 0; i < px; i++) {
                    dst[i] = rgb332_to_rgb565(rxBuf[i]);
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
                continue;
            }
            
            scale_180_to_240_table(rxBuf, dst, is_rgb565, src_lines);
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
        // 检查缓冲区是否足够 - 使用动态计算
        if (dst_lines > RGB_LINE_BATCH) {
            // 如果超出，调整接收到的行数
            int max_src_lines = RGB_LINE_BATCH / 2;
            if (src_lines > max_src_lines) {
                src_lines = max_src_lines;
                dst_lines = max_src_lines * 2;
            }
        }
        
        scale_120_to_240(rxBuf, dst, is_rgb565, src_lines);
        }

        // ------------------ 提交 ------------------
        f->y_start = dst_y0;
        f->line_count = dst_lines;
        f->state = BUF_READY;
    }
}

// ================= Draw Frame =================
void drawFrame() {
    FrameData* newest = nullptr;
    int newestIdx = -1;

    // 找 READY 的缓冲区
    for (int i = 0; i < FRAME_BUF_COUNT; i++) {
        if (frameBuf[i].state == BUF_READY) {
            newest = &frameBuf[i];
            newestIdx = i;
            break;
        }
    }

    if (!newest) return;

    // 其余 READY 的直接丢弃（防堆积）
    // for (int i = 0; i < FRAME_BUF_COUNT; i++) {
    //     if (i != newestIdx && frameBuf[i].state == BUF_READY) {
    //         frameBuf[i].state = BUF_FREE;
    //     }
    // }

    newest->state = BUF_DISPLAYING;

    uint8_t nextDma = dmaSel ^ 1;

    // 等待 DMA 完成
    tft->dmaWait();

    memcpy(
        dmaBuf[nextDma],
        newest->lines,
        IMG_W * newest->line_count * 2
    );

    tft->startWrite();
    tft->pushImageDMA(
        0,
        newest->y_start,
        IMG_W,
        newest->line_count,
        dmaBuf[nextDma]
    );
    tft->endWrite();

    dmaSel = nextDma;
    newest->state = BUF_FREE;
    frameCount++;
}

// ================= Setup =================
void setup() {
    Serial.begin(115200);
    tft_init();
    
    tft->initDMA();
    tft->setSwapBytes(true);

    tft->setTextFont(1);      // 明确指定字体
    tft->setCursor(0, 4);    // ❗ 不要带第三个参数
    // 清屏并设置文字
    tft->fillScreen(TFT_BLACK);
    tft->setTextSize(2);
    tft->setTextColor(TFT_WHITE);
    
    // 显示文字
    tft->println("Waiting for WiFi...");
    
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(300);
        Serial.print(".");
    }
    tft->fillScreen(TFT_GREEN);
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

    // 创建 UDP 接收任务
    xTaskCreatePinnedToCore(
        udpReceiverTask,
        "udp_rx",
        8192,
        nullptr,
        2,
        nullptr,
        0
    );
    
    tft->fillScreen(TFT_BLACK);
    String wifi_str = WiFi.localIP().toString() + ":8888";
    tft->println(wifi_str);
    tft->setCursor(0,54);    // ❗ 不要带第三个参数
    tft->setTextFont(2);      // 明确指定字体
    tft->setTextSize(1);
    tft->println("Streaming client: https://github.com/tignioj/ESP32UDPScreenShareClient");
}

// ================= Debug Info =================
void printDebugInfo() {
    static uint32_t lastPrint = 0;
    uint32_t now = millis();
    
    if (now - lastPrint > 1000) {
        lastPrint = now;
        
        Serial.printf("内存: %u, ", ESP.getFreeHeap());
        Serial.printf("UDP包/秒: %u, ", udpPackets);
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
    }
}

// ================= Loop =================
void loop() {
    drawFrame();
    printDebugInfo();
}