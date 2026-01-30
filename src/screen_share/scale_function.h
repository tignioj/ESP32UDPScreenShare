#ifndef MY_SCALE_FUNCTION_H // 如果宏 MY_HEADER_H 没有被定义
#define MY_SCALE_FUNCTION_H // 那么定义这个宏，并编译下面的内容

#include <stdint.h>


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

#endif MY_SCALE_FUNCTION_H