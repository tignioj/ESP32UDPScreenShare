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
#define FRAME_BUF_COUNT 12
#define DMA_MAX_LINE 12

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
    uint8_t rxBuf[1460];

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

        bool src_is_rgb565 = (color_mode == 0);
        uint32_t src_bpp = src_is_rgb565 ? 2 : 1;
        uint32_t src_bytes = src_w * src_lines * src_bpp;

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

        if (udp.read(rxBuf, src_bytes) != src_bytes) {
            f->state = BUF_FREE;
            continue;
        }

        // ===== 统一转为 RGB565 =====
        f->src_y0    = src_y0;
        f->src_w     = src_w;
        f->src_lines = src_lines;

        uint16_t* dst = f->lines;
        

        if (src_is_rgb565) {
            memcpy(dst, rxBuf, src_w * src_lines * 2);
        } else {
            uint8_t* src = rxBuf;
            uint16_t* d = dst;
            int n = src_w * src_lines;

            // 指针展开+查表法，代码变得更加难懂了，此处目的是为了加快rgb332->rgb565的速度。
            // 效果立竿见影，从180us->55+us
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

    // unsigned long start_convert_time = micros();
    if (f->src_w == 240) {
        memcpy(dmaBuf[nextDma], f->lines, 240 * f->src_lines * 2);
        draw_y0 = f->src_y0;
        draw_lines = f->src_lines;
    }
    else if (f->src_w == 180) {
        scale_180_to_240_table(
             (uint8_t*)f->lines,
            dmaBuf[nextDma],
            true,
            f->src_lines
        );
        draw_y0 = (f->src_y0 * 240 + 120) / 180;
        draw_lines = (f->src_lines * 240 + 179) / 180;
    }
    else if (f->src_w == 120) {
        scale_120_to_240(
             (uint8_t*)f->lines,
            dmaBuf[nextDma],
            true,
            f->src_lines
        );
        draw_y0 = f->src_y0 * 2;
        draw_lines = f->src_lines * 2;
    }
    // Serial.printf("转换耗时%lu\n", micros()- start_convert_time);
    // 分 高  低
    //240 5   9
    //180 65  82(6行) 112(8行)
    //120 17  17
    tft->startWrite();
    tft->pushImageDMA(0, draw_y0, IMG_W, draw_lines, dmaBuf[nextDma]);
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
            // 这个缓冲主要用于接收udp包，udp包大小就这么大
            // UDP 包大小 ≠ lines 最终写入大小，转换颜色后，体积会变大！
            // 例如 rgb332 240分辨率一次发送6行，udp大小是1440，但是转换成565后，体积增大2倍
            // 如果你这里不*2，那么所有rgb332的数据都会花屏
            1460*2, 
            MALLOC_CAP_8BIT
        );
        frameBuf[i].state = BUF_FREE;
    }

    dmaBuf[0] = (uint16_t*)heap_caps_malloc(
        IMG_W * DMA_MAX_LINE * 2, // 要求发送端发送的行数放大后不得超过MAXLINE否则会报错重启
        MALLOC_CAP_DMA
    );
    dmaBuf[1] = (uint16_t*)heap_caps_malloc(
        IMG_W * DMA_MAX_LINE * 2,
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
