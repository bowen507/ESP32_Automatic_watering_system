#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#define CURRENT_VERSION "1.0.3"

// OTA 固件升级总开关（默认关闭）
// 不影响 version.json / settings.json 的远程设置拉取
#define OTA_ENABLED false

// 使用 RTC 内存存储的变量
extern RTC_DATA_ATTR int wakeupCount;
extern RTC_DATA_ATTR bool isWateringDay;

// 应用状态变量
extern bool pumpStatus;
extern bool powerSavingMode; // 节电模式
extern int moistureValue;
extern int moisturePercent;
struct AppSettings {
    int waterTimeSeconds; // 浇水时长(秒)
    int sleepHours;       // 休眠时长(小时)
    int sleepMinutes;     // 休眠时长(分钟)
};

// 显示更新间隔
extern const int DISPLAY_UPDATE_INTERVAL;

#endif