#ifndef FUNCTION_DECLARATIONS_H
#define FUNCTION_DECLARATIONS_H

#include <Arduino.h> // 提供 String 类型

// 各个功能函数的声明
void enterDeepSleep(uint64_t microseconds);//进入深度睡眠
void button1_callback(const String &state);//按钮浇水
void dataStorage();//数据存储上传

#endif