#include "oled_ctrl.h"
#include <Wire.h>

DisplayManager::DisplayManager() : display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET) {
}

void DisplayManager::begin() {
    Wire.begin(SDA_PIN, SCL_PIN);
    if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        Serial.println(F("SSD1306 allocation failed"));
        for(;;);
    }
    display.clearDisplay();
    display.display();
}

void DisplayManager::update(int moisturePercent, int rssi, bool pumpStatus, int wakeCount) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0,0);
    display.print("Moisture: ");
    display.print(moisturePercent);
    display.println("%");
    display.print("WiFi: ");
    display.print(rssi);
    display.println(" dBm");
    display.print("Pump: ");
    display.println(pumpStatus ? "ON" : "OFF");
    display.print("WakeCount: ");
    display.println(wakeCount);
    display.display();
}

void DisplayManager::setPower(bool on) {
    if (on) {
        display.ssd1306_command(SSD1306_DISPLAYON);
    } else {
        display.ssd1306_command(SSD1306_DISPLAYOFF);
    }
}

// 菜单系统专用显示函数
void DisplayManager::showMenu(const String& title, const String items[], int itemCount, int selectedIndex) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);

    // 显示标题
    display.setCursor(0, 0);
    display.println(title);

    // 显示分隔线
    display.drawFastHLine(0, 10, SCREEN_WIDTH, SSD1306_WHITE);

    // 显示菜单项
    for (int i = 0; i < itemCount && i < 5; i++) { // 最多显示5项
        display.setCursor(5, 15 + i * 10);
        if (i == selectedIndex) {
            display.print("> ");
        } else {
            display.print("  ");
        }
        display.println(items[i]);
    }
    display.display();
}

void DisplayManager::showMessage(const String& message) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println(message);
    display.display();
}

// 基础显示功能（供高级菜单使用）
void DisplayManager::clear() { display.clearDisplay(); }
void DisplayManager::setTextSize(uint8_t size) { display.setTextSize(size); }
void DisplayManager::setTextColor(uint16_t color) { display.setTextColor(color); }
void DisplayManager::setCursor(int16_t x, int16_t y) { display.setCursor(x, y); }
void DisplayManager::print(const String &text) { display.print(text); }
void DisplayManager::println(const String &text) { display.println(text); }
void DisplayManager::displayBuffer() { display.display(); }