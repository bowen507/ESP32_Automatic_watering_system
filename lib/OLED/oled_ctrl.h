#ifndef OLED_CTRL_H
#define OLED_CTRL_H

#include <Adafruit_SSD1306.h>
#include <Adafruit_GFX.h>
#include "pins_config.h" // 包含引脚定义

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

class DisplayManager {
private:
    Adafruit_SSD1306 display;

public:
    DisplayManager();
    void begin();

    // 主显示函数
    void update(int moisturePercent, int rssi, bool pumpStatus, int wakeCount);

    // 电源控制
    void setPower(bool on);

    // 为菜单系统提供的公共接口
    void clear();// 清屏
    void setTextSize(uint8_t size);// 设置文本大小
    void setTextColor(uint16_t color);// 设置文本颜色
    void setCursor(int16_t x, int16_t y);// 设置光标位置
    void print(const String &text);// 打印文本
    void println(const String &text);// 打印文本并换行
    void displayBuffer();// 显示缓冲区内容

    // 菜单专用显示函数
    void showMenu(const String& title, const String items[], int itemCount, int selectedIndex);
    void showMessage(const String& message);
};

#endif