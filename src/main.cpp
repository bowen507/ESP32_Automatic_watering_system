// =============================================================================
// ESP32 智能灌溉系统 — 主程序
// =============================================================================
//
// 运行模式：
//   setup() 内完成全部工作 → 深睡 → 定时唤醒 → 重复
//   loop() 保持空闲，所有逻辑在 setup() 中顺序执行
//
// 唤醒流程（setup 内顺序）：
//   1. 初始化硬件引脚、OLED、菜单
//   2. 从 NVS 恢复持久化配置（跨深睡 / 跨 OTA）
//   3. 按策略决定本次是否联网 → 联网则同步 NTP、拉取远程设置
//   4. 递增唤醒计数 → 判断是否为浇水日 → 执行浇水
//   5. 读取土壤湿度 → 上报 Blinker（在线时）
//   6. 维护日 OTA 检查与下载（仅在线时）
//   7. 日志处理（每次唤醒都执行：在线上传，离线缓存 NVS）
//   8. 保持唤醒窗口（用户按键交互、数据刷新）
//   9. 计算深睡时长 → 保存预计唤醒时间 → 进入深睡
//
// 节电模式 vs 普通模式：
//   - 节电：12h 唤醒一次，偶数次联网，每日一次 OTA + 日志
//   - 普通：按设定时长休眠，始终联网，每 4 次唤醒做一次 OTA
//
// 关键依赖：
//   - Blinker（物联网）：数据上报、远程控制
//   - Gitee API：OTA 固件、版本检测、日志文件上传
//   - NVS：跨深睡 & 跨 OTA 的持久化配置
//   - RTC_DATA_ATTR：深睡不丢失的全局变量
// =============================================================================

#define BLINKER_WIFI
#define RTC_MEM_ADDR 64

// ---- 项目头文件 ----
#include "pins_config.h"
#include "blinker_config.h"
#include "app_config.h"
#include "function_declarations.h"
#include "network_manager.h"
#include "oled_ctrl.h"
#include "menu_manager.h"
#include "log_manager.h"
#include "nvs_ctrl.h"
#include "secrets.h"

// ---- 框架 / 库头文件 ----
#include <WiFi.h>
#include <Blinker.h>
#include <time.h>
#include <esp_sleep.h>
#include "git_ctrl.h"
#include <esp_ota_ops.h>

// =============================================================================
// 全局对象
// =============================================================================

// --- OTA 与版本管理 ---
GitCtrl gitCtrl(SECRET_GITEE_TOKEN, SECRET_GITEE_USER, SECRET_GITEE_REPO);

// --- 网络凭据（定义于 secrets.h） ---
uint8_t stationMAC[] = SECRET_STATION_MAC;
char auth[] = SECRET_BLINKER_AUTH;
char ssid[] = SECRET_WIFI_SSID;
char pswd[] = SECRET_WIFI_PASS;

// --- Blinker 组件（物联网平台数据通道与控件） ---
BlinkerNumber HUMI("humi");               // 湿度数值
BlinkerNumber WLAN("wlan");               // WiFi 信号强度
BlinkerText WaterTime("water_time");      // 浇水时间文本
BlinkerText SleepMode("tex-sleep");       // 休眠状态文本
BlinkerButton Button1("btn-abc");         // 手动浇水按钮
BlinkerButton ButtonSleep("btn-sleep");   // 阻止休眠开关

// Blinker 心跳：在阻塞操作中维持连接
static void blinkerKeepAlive() {
    Blinker.run();
}

// --- 本地外设 ---
DisplayManager oled;              // SSD1306 OLED 显示
MenuManager menu(&oled);          // 三级菜单系统（按键交互）
LogManager logManager;            // 日志生成 / 缓存 / 上传

// =============================================================================
// RTC_DATA_ATTR 变量 — 深睡唤醒后值不丢失
// =============================================================================

RTC_DATA_ATTR int wakeupCount = 0;                // 唤醒计数器（用于浇水周期 & 日志）
RTC_DATA_ATTR bool powerSavingMode = true;         // 节电模式开关
RTC_DATA_ATTR bool isWateringDay = false;          // 本次是否为浇水日
RTC_DATA_ATTR AppSettings globalSettings = {5, 6, 0}; // {浇水秒, 休眠时, 休眠分}
RTC_DATA_ATTR bool settingonline = true;           // 远程设置总开关

int* getWakeupCountPtr() {
    return &wakeupCount;
}

// =============================================================================
// 非持久化变量 — 每次唤醒重新初始化
// =============================================================================

bool pumpStatus = false;           // 水泵当前状态（开/关）
bool preventSleep = false;         // 用户手动阻止休眠标记
const int moisturePin = SOIL_PIN;  // 土壤湿度传感器 ADC 引脚
int moistureValue;                 // ADC 原始值
int moisturePercent;               // 映射后的 0-100% 湿度百分比

NetworkManager nm(auth, ssid, pswd, stationMAC);

// =============================================================================
// Blinker 回调函数
// =============================================================================

// 手动浇水按钮：水泵开启 5 秒
void button1_callback(const String &state) {
    digitalWrite(PUMP_PIN, HIGH);
    pumpStatus = true;
    unsigned long startMillis = millis();

    while (millis() - startMillis < 5000) {
        Blinker.run();  // 浇水期间维持 Blinker 心跳
    }

    digitalWrite(PUMP_PIN, LOW);
    pumpStatus = false;
    String startTime = nm.getCurrentTime();
    WaterTime.print("浇水时间: " + startTime + " CST");
    Serial.println("上报时间: " + startTime);
}

// 阻止休眠按钮：切换 preventSleep 标记
void buttonSleep_callback(const String &state) {
    preventSleep = !preventSleep;
    if (preventSleep) {
        SleepMode.print("已阻止休眠，设备将保持唤醒");
        Serial.println("Sleep prevented by user.");
    } else {
        SleepMode.print("已取消阻止，设备将按计划休眠");
        Serial.println("Sleep prevention cancelled.");
    }
}

// Blinker 数据存储回调：将数据推送到云端存储
void dataStorage() {
    Blinker.dataStorage("temp", moisturePercent);
    Blinker.dataStorage("timecount", wakeupCount);
}

// =============================================================================
// setup() — 设备主入口（每次唤醒执行一次，完成后深睡）
// =============================================================================

void setup() {
    unsigned long startSetupMillis = millis();

    // ---- 1. 基础初始化 ----

    Serial.begin(115200);
    Serial.println("\nREADY");

    // 统一时区为东八区，确保离线时 localtime 不按 UTC 显示
    setenv("TZ", "CST-8", 1);
    tzset();

    // ---- 2. 硬件引脚初始化 ----

    pinMode(LED_BUILTIN, OUTPUT);
    pinMode(moisturePin, INPUT);
    pinMode(SENSOR_POWER_PIN, OUTPUT);
    digitalWrite(SENSOR_POWER_PIN, LOW);   // 传感器默认断电
    pinMode(PUMP_PIN, OUTPUT);
    digitalWrite(LED_BUILTIN, HIGH);        // LED 默认亮
    digitalWrite(PUMP_PIN, LOW);            // 水泵默认关

    Blinker.attachDataStorage(dataStorage);

    // 标记当前固件有效，取消可能存在的 OTA 回滚
    esp_ota_mark_app_valid_cancel_rollback();

    // ---- 3. 显示与菜单初始化 ----

    oled.begin();
    menu.begin();
    menu.bindSettings(&globalSettings, &wakeupCount, &powerSavingMode);

    // ---- 4. 从 NVS 恢复持久化状态 ----
    // loadConfig: 恢复常驻配置（覆盖 RTC 默认值）
    // restoreAfterOta: 若上次是 OTA 后启动，恢复 OTA 前保存的运行时状态
    // applyPredictedTimeIfInvalid: 离线时用上次深睡前推算的时间恢复系统时钟

    NvsCtrl::loadConfig(settingonline, powerSavingMode, globalSettings);
    if (NvsCtrl::restoreAfterOta(wakeupCount, powerSavingMode, globalSettings)) {
        Serial.println("[System] Restoring state from NVS after OTA...");
    }

    if (NvsCtrl::applyPredictedTimeIfInvalid()) {
        Serial.println("[Time] Applied predicted wake time from NVS");
    }

    // ---- 5. 联网（按策略决定）----

    bool policyShouldConnect = false;
    bool isOnline = nm.beginByPolicy(powerSavingMode, wakeupCount, policyShouldConnect);
    if (isOnline) {
        Blinker.begin(auth);
    }

    // ---- 6. 唤醒计数 & 浇水判断 ----

    wakeupCount++;
    menu.bindSettings(&globalSettings, &wakeupCount, &powerSavingMode);

    int wateringThreshold = powerSavingMode ? 6 : 12;

    if (wakeupCount >= wateringThreshold) {
        isWateringDay = true;
        wakeupCount = 0;
    }

    // ---- 7. Blinker 按钮绑定 & 首次心跳 ----

    Button1.attach(button1_callback);
    ButtonSleep.attach(buttonSleep_callback);

    if (isOnline) {
        Blinker.run();
    }

    // ---- 8. 土壤湿度采样 ----
    // 传感器由 GPIO 供电：采样时通电 → 短暂延迟 → 读取 → 断电

    digitalWrite(SENSOR_POWER_PIN, HIGH);
    delay(20);
    moistureValue = analogRead(moisturePin);
    digitalWrite(SENSOR_POWER_PIN, LOW);
    moisturePercent = map(moistureValue, 2000, 1000, 0, 100);

    // ---- 9. 自动浇水 ----

    if (isWateringDay) {
        digitalWrite(PUMP_PIN, HIGH);
        delay(globalSettings.waterTimeSeconds * 1000);
        digitalWrite(PUMP_PIN, LOW);

        String startTime = nm.getCurrentTime();
        WaterTime.print("浇水时间: " + startTime + " CST");
        isWateringDay = false;
    }

    // ---- 10. 上报 Blinker ----

    if (isOnline) {
        HUMI.print(moisturePercent);
        WLAN.print(WiFi.RSSI());
    }

    Serial.printf("Moisture: %d %%, Wakeups: %d\n", moisturePercent, wakeupCount);

    // ---- 11. 每日维护：OTA 检查与下载 ----
    //
    // OTA_ENABLED 总开关（app_config.h），默认关闭
    // 关闭时仅跳过固件下载，不影响 version.json / settings.json 远程设置

    bool isUpdateAvailable = false;
    String newVersion = "";
    bool otaChecked = false;

#if OTA_ENABLED
    bool triggerMaintenance = false;
    if (wakeupCount == 1) {
        triggerMaintenance = true;  // 首次唤醒总是做一次 OTA 检查
    } else if (powerSavingMode) {
        triggerMaintenance = policyShouldConnect;
    } else {
        triggerMaintenance = (wakeupCount % 4 == 0);
    }

    if (triggerMaintenance && isOnline) {
        Serial.println("====== Daily Check Routine ======");
        gitCtrl.setupOTA(SECRET_VERSION_URL, CURRENT_VERSION);

        String firmwareUrl = "";
        newVersion = gitCtrl.detectNewVersion(firmwareUrl);
        isUpdateAvailable = !newVersion.isEmpty();
        otaChecked = true;

        if (isUpdateAvailable) {
            Serial.println("[System] Update found. Saving state to NVS...");
            NvsCtrl::saveOtaState(wakeupCount, powerSavingMode, globalSettings);

            if (!gitCtrl.performUpdate(firmwareUrl)) {
                Serial.println("[System] OTA Update Failed! Cleaning up NVS flags...");
                NvsCtrl::clearOtaFlag();
                nm.resetWiFi();
            }
        }
    }
#else
    Serial.println("[System] OTA disabled (OTA_ENABLED=false)");
#endif

    // ---- 12. 远程设置拉取（在 OTA 之后，日志之前） ----
    // 在线时尝试拉取 version.json → settings.json → 覆盖本地参数 → 写入 NVS
    nm.applyOnlineSettingsIfEnabled(gitCtrl, isOnline, SECRET_VERSION_URL, SECRET_SETTINGONLINE_URL,
                                    settingonline, powerSavingMode, globalSettings);

    // ---- 日志处理（每次唤醒都执行） ----
    // 在线：上传当前日志 + 补传 NVS 中的缓存日志
    // 离线：生成日志并追加写入 NVS，待下次联网补传

    logManager.processLogs(gitCtrl, wakeupCount, isUpdateAvailable, newVersion, moisturePercent, isOnline,
                           otaChecked, settingonline, powerSavingMode, globalSettings,
                           isOnline ? blinkerKeepAlive : nullptr);

    // ---- 14. 唤醒窗口：用户交互 & 数据刷新 ----
    //
    // 在线唤醒 120 秒，离线唤醒 30 秒（前提：preventSleep 为 false）
    // 期间响应按键操作菜单、每秒刷新传感器读数并更新 OLED / Blinker

    unsigned long wasteMillis = millis() - startSetupMillis;
    unsigned long lastPrintTime = 0;
    unsigned long wakeWindowStart = millis();

    long targetWakeDuration = isOnline ? 120000 : 30000;

    // 防止 setup 耗时超过窗口导致下溢
    int64_t remainingWakeMs = (int64_t)targetWakeDuration - (int64_t)wasteMillis;
    if (remainingWakeMs < 0) {
        remainingWakeMs = 0;
    }

    while (preventSleep || ((int64_t)(millis() - wakeWindowStart) < remainingWakeMs)) {
        // 心跳维持
        if (isOnline) {
            Blinker.run();
        }

        // 按键处理 & 菜单逻辑
        if (menu.handleButtonInput()) {
            menu.update(moisturePercent, WiFi.RSSI(), pumpStatus, wakeupCount);

            // 仅菜单真正修改了配置时才写入 NVS，避免频繁擦写
            if (menu.consumeSettingsDirty()) {
                NvsCtrl::saveConfig(settingonline, powerSavingMode, globalSettings);
                Serial.println("[Settings] Saved menu settings to NVS");
            }
        }

        // 每秒刷新传感器读数
        if (millis() - lastPrintTime >= 1000) {
            lastPrintTime = millis();

            digitalWrite(SENSOR_POWER_PIN, HIGH);
            delay(20);
            moistureValue = analogRead(moisturePin);
            digitalWrite(SENSOR_POWER_PIN, LOW);
            moisturePercent = map(moistureValue, 2000, 1000, 0, 100);

            // 在线时才推送 Blinker，避免离线触发 TLS 连接导致报错
            if (isOnline && WiFi.status() == WL_CONNECTED) {
                HUMI.print(moisturePercent);
                WLAN.print(WiFi.RSSI());
            }

            menu.update(moisturePercent, WiFi.RSSI(), pumpStatus, wakeupCount);
        }
    }

    // ---- 15. 计算深睡时长 & 进入深睡 ----

    uint64_t sleepTime;
    if (powerSavingMode) {
        sleepTime = 12 * 3600 * 1e6;  // 节电模式：12 小时（微秒）
    } else {
        sleepTime = (globalSettings.sleepHours * 3600 + globalSettings.sleepMinutes * 60) * 1e6;
    }

    oled.setPower(false);                    // 息屏省电
    digitalWrite(SENSOR_POWER_PIN, LOW);     // 传感器断电

    // 扣除已消耗的唤醒窗口时间，防止下溢
    int64_t calculatedSleepTime = (int64_t)sleepTime
                                - (int64_t)(targetWakeDuration * 1000)
                                - (int64_t)(wasteMillis * 1000);

    if (calculatedSleepTime <= 0) {
        Serial.printf("[Warning] Calculated sleep time is %lld, resetting to 10s minimum.\n",
                      calculatedSleepTime);
        calculatedSleepTime = 10 * 1000000LL;
    }

    // 保存预计唤醒时间（Epoch 秒），供下次离线唤醒时恢复相对可用的系统时间
    time_t nowEpoch = time(nullptr);
    if (nowEpoch > 0) {
        uint64_t nextEpoch = (uint64_t)nowEpoch + (uint64_t)((uint64_t)calculatedSleepTime / 1000000ULL);
        NvsCtrl::setPredictedWakeEpoch(nextEpoch);
    }

    nm.enterDeepSleep((uint64_t)calculatedSleepTime);
}

void loop() {
    // 所有操作在 setup() 中一次性完成，然后进入深睡，不会执行到此
}
