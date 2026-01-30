#include "common.h"

#include <TFT_eSPI.h>

void setBackLight(float duty)
{
    duty = constrain(duty, 0, 1);
    duty = 1 - duty;
    ledcWrite(LCD_BL_PWM_CHANNEL, (int)(duty * 255));
}
/*
TFT pins should be set in path/to/Arduino/libraries/TFT_eSPI/User_Setups/Setup24_ST7789.h
*/
TFT_eSPI *tft = new TFT_eSPI(SCREEN_HOR_RES, SCREEN_VER_RES);

void tft_init() {
    ledcSetup(LCD_BL_PWM_CHANNEL, 5000, 8);
    ledcAttachPin(LCD_BL_PIN, LCD_BL_PWM_CHANNEL);

    setBackLight(0.0); // 设置亮度 为了先不显示初始化时的"花屏"

    tft->begin(); /* TFT init */
    tft->fillScreen(TFT_BLACK);
    tft->writecommand(ST7789_DISPON); // Display on

    tft->setRotation(4); /* mirror 修改反转，如果加上分光棱镜需要改为4镜像*/

    setBackLight(100 / 100.0); // 设置亮度

}