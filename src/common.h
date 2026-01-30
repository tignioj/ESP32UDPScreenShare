// 在 my_header.h 文件的开头这样写
#ifndef MY_COMMON_H // 如果宏 MY_HEADER_H 没有被定义
#define MY_COMMON_H // 那么定义这个宏，并编译下面的内容

#include "secrets.h"  // 敏感数据

// RGB
#define RGB_LED_PIN 27

// SD_Card
#define SD_SCK 14
#define SD_MISO 26
#define SD_MOSI 13
#define SD_SS 15

// MUP6050
#define IMU_I2C_SDA 32
#define IMU_I2C_SCL 33



// 屏幕尺寸
#define SCREEN_HOR_RES 240 // 水平
#define SCREEN_VER_RES 240 // 竖直

#define SCREEN_HEIGHT SCREEN_VER_RES
#define SCREEN_WIDTH SCREEN_HOR_RES

// TFT屏幕接口
#define LCD_BL_PIN 5

#define LCD_BL_PWM_CHANNEL 0


#include <TFT_eSPI.h>
/*
TFT pins should be set in path/to/Arduino/libraries/TFT_eSPI/User_Setups/Setup24_ST7789.h
*/
extern TFT_eSPI *tft;
void setBackLight(float);
void tft_init();



#endif
