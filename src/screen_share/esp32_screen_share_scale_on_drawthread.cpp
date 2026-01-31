#include <WiFi.h>
#include <WiFiUdp.h>
#include "common.h"
#include "scale_function.h"

// ================= WiFi =================
const char* ssid = WIFI_SSID_STR;
const char* password = WIFI_PASSWORD_STR;

WiFiUDP udp;
#define UDP_PORT 8888

// ================= Image =================
#define IMG_W 240
#define FRAME_BUF_COUNT 16

// ================= Frame Buffer =================
enum BufState {
    BUF_FREE,
    BUF_FILLING,
    BUF_READY,
    BUF_DISPLAYING
};

struct FrameData {
    uint16_t src_y0;
    uint8_t  src_w;
    uint8_t  src_lines;
    bool     is_rgb565;
    uint16_t *lines;          // 原始行数据（RGB565）
    volatile BufState state;
};

FrameData* frameBuf = nullptr;

// ================= DMA =================
uint16_t* dmaBuf[2];
volatile uint8_t dmaSel = 0;

// ================= Stats =================
volatile uint32_t frameCount = 0;
volatile uint32_t dropCount  = 0;
volatile uint32_t udpPackets = 0;

// ======================================================
//                    UDP Receiver
// ======================================================
void udpReceiverTask(void* param)
{
    uint8_t rxBuf[1460]; // 一个udp包的大小最多1460，大了parsePacket()会收不到数据
    while (1) {
        int packetSize = udp.parsePacket();
        if (packetSize <= 0) {
            vTaskDelay(1);
            continue;
        }
        udpPackets++;

        uint8_t header[5];
        if (udp.read(header, 5) != 5) {
            udp.flush();
            continue;
        }

        uint16_t src_y0 = (header[2] << 8) | header[3];
        uint8_t flags   = header[4];

        uint8_t resolution = (flags >> 6) & 0x03;
        uint8_t color_mode = (flags >> 4) & 0x03;
        uint8_t src_lines  = flags & 0x0F;

        if (src_lines == 0 || src_lines > 8) {
            udp.flush();
            continue;
        }

        int src_w;
        switch (resolution) {
            case 0: src_w = 240; break;
            case 1: src_w = 180; break;
            case 2: src_w = 120; break;
            default:
                udp.flush();
                continue;
        }

        bool is_rgb565 = (color_mode == 0);
        uint32_t bytes_per_px = is_rgb565 ? 2 : 1;
        uint32_t expect = src_w * src_lines * bytes_per_px;

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

        // ===== 只做格式统一，不做 scale =====
        f->src_y0   = src_y0;
        f->src_w    = src_w;
        f->src_lines = src_lines;
        f->is_rgb565 = is_rgb565; // 下面做了颜色转化所以这里可以直接设置为True

        memcpy(f->lines, rxBuf, expect);
        // if (is_rgb565) {
        //     memcpy(f->lines, rxBuf, src_w * src_lines * 2);
        // } else {
        //     // 逐个像素点转换。
        //     //这里w*h是因为在传送图像的二维数组实际上被平摊成了一维数组，
        //     // 所以i从0到w*h就可以遍历所有像素点。
        //     for (int i = 0; i < src_w * src_lines; i++) {
        //         f->lines[i] = rgb332_to_rgb565(rxBuf[i]);
        //     }
        // }
        f->state = BUF_READY;
    }
}

// ======================================================
//                    Draw Frame (唯一 scale 点)
// ======================================================
void drawFrame()
{
    FrameData* f = nullptr;
    for (int i = 0; i < FRAME_BUF_COUNT; i++) {
        if (frameBuf[i].state == BUF_READY) {
            f = &frameBuf[i];
            break;
        }
    }
    if (!f) return;

    f->state = BUF_DISPLAYING;
    uint8_t nextDma = dmaSel ^ 1;

    tft->dmaWait();

    int draw_y0 = 0;
    int draw_lines = 0;

    // ================= 240 =================
    if (f->src_w == 240) {
        if(f->is_rgb565){
            memcpy(dmaBuf[nextDma], f->lines, 240 * f->src_lines * 2);
        }
        else {
            uint16_t* dst = dmaBuf[nextDma];
            uint8_t*  src = (uint8_t*)f->lines;

            int px = 240 * f->src_lines;
            for (int i = 0; i < px; i++) {
                dst[i] = rgb332_to_rgb565(src[i]);
            }
        }
        draw_y0 = f->src_y0;
        draw_lines = f->src_lines;
    }
    // ================= 180 → 240 =================
    else if (f->src_w == 180) {
        // 根据linecount 计算ystart和绘制行数
        scale_180_to_240_table(
            (uint8_t*)f->lines,
            dmaBuf[nextDma],
            f->is_rgb565,
            f->src_lines
        );
        draw_y0 = (f->src_y0 * 240 + 120) / 180;
        draw_lines = (f->src_lines * 240 + 179) / 180;
    }
    // ================= 120 → 240 =================
    else if (f->src_w == 120) {
        scale_120_to_240(
            (uint8_t*)f->lines,
            dmaBuf[nextDma],
            f->is_rgb565,
            f->src_lines
        );
        draw_y0 = f->src_y0 * 2;
        draw_lines = f->src_lines * 2;
    }

    tft->startWrite();
    tft->pushImageDMA(
        0,
        draw_y0,
        IMG_W,
        draw_lines,
        dmaBuf[nextDma]
    );
    tft->endWrite();

    dmaSel = nextDma;
    f->state = BUF_FREE;
    frameCount++;
}

// ======================================================
//                    Init
// ======================================================
void initdata()
{
    frameBuf = (FrameData*)calloc(FRAME_BUF_COUNT, sizeof(FrameData));
    for (int i = 0; i < FRAME_BUF_COUNT; i++) {
        frameBuf[i].lines = (uint16_t*)heap_caps_malloc(
            1460,
            MALLOC_CAP_8BIT
        );
        frameBuf[i].state = BUF_FREE;
    }

    dmaBuf[0] = (uint16_t*)heap_caps_malloc(
        IMG_W * 16 * 2,
        MALLOC_CAP_DMA
    );
    dmaBuf[1] = (uint16_t*)heap_caps_malloc(
        IMG_W * 16 * 2,
        MALLOC_CAP_DMA
    );

    init_scale_maps();
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
// ======================================================
//                    Setup / Loop
// ======================================================
void setup()
{
    Serial.begin(115200);
    tft_init();
    tft->initDMA();
    tft->setSwapBytes(true);

    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) delay(200);

    udp.begin(UDP_PORT);
    initdata();

    xTaskCreatePinnedToCore(
        udpReceiverTask,
        "udp_rx",
        8192,
        nullptr,
        2,
        nullptr,
        0
    );
}

void loop()
{
    drawFrame();
    printDebugInfo();
}
