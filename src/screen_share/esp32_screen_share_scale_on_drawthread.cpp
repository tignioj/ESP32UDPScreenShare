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
#define FRAME_BUF_COUNT 6 // 主要用来接收udp包的缓冲
#define MAX_DMA_LINECOUNT 12  // DMA缓冲区大小 要求发送端发送的行数放大后不得超过MAX_DMA_LINECOUNT否则会报错重启

// ================= Frame Buffer =================
enum BufState {
    BUF_FREE,
    BUF_FILLING,
    BUF_READY,
    BUF_DISPLAYING
};

struct FrameData {
    uint16_t frame_id;
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
        uint16_t frame_id = (header[0] << 8) | header[1];
        uint16_t src_y0 = (header[2] << 8) | header[3];
        uint8_t  flags   = header[4];

        uint8_t resolution = (flags >> 6) & 0x03;
        uint8_t color_mode = (flags >> 4) & 0x03;
        uint8_t src_lines  = flags & 0x0F;

        if (src_lines == 0 || src_lines > 8) { // 大于8行的直接丢掉
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

        if (!f) { // TODO 缓存满了，要不要考虑覆盖最旧的帧？
            dropCount++;
            udp.flush();
            continue;
        }

        if (udp.read(rxBuf, expect) != expect) {
            f->state = BUF_FREE;
            continue;
        }

        // ===== 只做格式统一，不做 scale =====
        f->frame_id = frame_id;
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
    // 画第一个准备好的
    // for (int i = 0; i < FRAME_BUF_COUNT; i++) {
    //     if (frameBuf[i].state == BUF_READY) {
    //         f = &frameBuf[i];
    //         break;
    //     }
    // }
    unsigned int min_idx = -1;
    unsigned int min_val = 0;
    unsigned long count_time = micros();
    for (int i = 0; i < FRAME_BUF_COUNT; i++) {
        if (frameBuf[i].state == BUF_READY) {
            min_idx = i; // 先找到第一个ready的作为比较最小值的根基
            min_val = frameBuf[i].src_y0 + frameBuf[i].frame_id*2; // 提高id权重
        }
    }
    // 所有frame_id最小的之中y_id最小的
    for (int i = 0; i < FRAME_BUF_COUNT; i++) {
        if (frameBuf[i].state == BUF_READY) {

            if(frameBuf[i].src_y0 + frameBuf[i].frame_id*2 < min_val){
                min_val = frameBuf[i].src_y0 + frameBuf[i].frame_id*2; // 提高id权重
                min_idx = i;
            } 
        }
    }
    if (min_idx==-1) return;
    f = &frameBuf[min_idx];
    // Serial.printf("计算耗时%lu\n", micros() - count_time); // 4us

    f->state = BUF_DISPLAYING;
    uint8_t nextDma = dmaSel ^ 1;
    tft->dmaWait(); // 这个最多1微秒

    int draw_y0 = 0;
    int draw_lines = 0;

    // unsigned long start_convert_time = micros();
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
    // TODO 颜色转换放到UDP线程
    // Serial.printf("转换花费微秒:%d \n", micros() - start_convert_time);
    // 实测耗费时间: 
    // 240低彩223, 高彩9
    // 180低彩:630， 高彩90
    // 120低彩187， 高彩52
    // 可见颜色转换就废了不少时间。
    // unsigned long start_draw_time = micros();

    tft->startWrite();
    tft->pushImageDMA(
        0,
        draw_y0,
        IMG_W,
        draw_lines,
        dmaBuf[nextDma]
    );
    tft->endWrite();
    // Serial.printf("绘制花费间%d\n", micros() - start_draw_time);
    // 绘制耗时统计
    // 240-3行 388us
    // 240-6行 700us
    // 180-4(会放大到6) 700us
    // 180-8(会放大到12) 1235 
    // 120-6(会放大到12) 1340
    // 120-8(会放大到16) 1770
    // 120-10(会放大到20) 2190
    // 120-12(会放大到24) 2615

    // 统计总耗时
    // item    放大/复制/转换色彩 绘制 多少次满一帧    总计(微秒)
    // 240高彩:         9           9   240/3=80    80*18=     1440
    // 240低彩:        223         388  240/6=40    40*611=    24440
    // 180高彩:         90         700  180/4=45    45*790=    35550
    // 180低彩:        630        1235  180/8=22.5  22.5*1865= 41962.5  
    // 120高彩:        53         1340  120/6=20    20*1395=   27900
    // 120低彩:        187        1340  120/6=20    20*1527=   30540
    // 120低彩:        187        1770  120/8=15    15*1957=   29355
    // 120低彩:        187        2190  120/10=12   12*2337=   28524
    // 120低彩:        187        2615  120/12=10   10*15527=  28020
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
            1460, // 这个大小主要是MTU单元大小
            MALLOC_CAP_8BIT
        );
        frameBuf[i].state = BUF_FREE;
    }

    dmaBuf[0] = (uint16_t*)heap_caps_malloc(
        //要求发送端发送的行数放大后不得超过MAX_DMA_LINECOUNT否则会报错重启
        IMG_W * MAX_DMA_LINECOUNT * 2,  
        MALLOC_CAP_DMA
    );
    dmaBuf[1] = (uint16_t*)heap_caps_malloc(
        IMG_W * MAX_DMA_LINECOUNT * 2,
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
        // Serial.print("|");
        // for (int i = 0; i < FRAME_BUF_COUNT; i++) {
        //     if(frameBuf[i].state == BUF_READY) {
        //         Serial.printf("%4d,%3d;", frameBuf[i].frame_id, frameBuf[i].src_y0);
        //     }
        // }
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
