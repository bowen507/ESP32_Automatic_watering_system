
#ifndef PINS_CONFIG_H
#define PINS_CONFIG_H

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1    // OLED复位引脚（-1表示不使用）

#define PUMP_PIN 27      // 水泵控制引脚
#define SOIL_PIN 32      // 土壤湿度传感器引脚
#define SENSOR_POWER_PIN 25 // 传感器供电引脚

#define SCL_PIN 22       // I2C时钟引脚
#define SDA_PIN 21       // I2C数据引脚

#define BTN_UP_PIN     13    // 上键
#define BTN_SELECT_PIN 14    // 选择键
#define BTN_DOWN_PIN   15    // 下键

#endif