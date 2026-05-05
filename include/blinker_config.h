#ifndef BLINKER_CONFIG_H
#define BLINKER_CONFIG_H

#include <Blinker.h> // 包含必要的库，确保声明有效

// Blinker 配置信息
extern char auth[];  // 使用 extern 关键字进行声明，而非定义
extern char ssid[];
extern char pswd[];

// 蓝牙MAC地址（固定）
extern uint8_t stationMAC[6];

// 声明Blinker组件对象（extern 表示它们在其他地方定义）
extern BlinkerNumber HUMI;
extern BlinkerNumber WLAN;
extern BlinkerText WaterTime;
extern BlinkerButton Button1;

#endif