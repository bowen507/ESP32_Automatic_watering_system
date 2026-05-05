// =============================================================================
// NVS 持久化控制器 — Preferences 封装
// =============================================================================
//
// NVS 中两类数据的生命周期：
//
//   常驻配置（loadConfig / saveConfig）：
//     跨深睡 + 跨 OTA 保留，仅菜单修改或远程 settings.json 下发时更新。
//     - settingonline：远程设置总开关
//     - powerSavingMode：节电模式
//     - globalSettings：浇水与休眠参数（AppSettings 结构体）
//     - settings_rev：已应用的远程设置修订号
//
//   临时状态（saveOtaState / restoreAfterOta）：
//     OTA 升级前保存运行时状态，升级后恢复。OTA 完成或失败后清除。
//     - wakeupCount / 节电模式 / AppSettings 的临时副本
//     - ota_flag：标记是否存在待恢复的临时状态
//
//   日志缓存（getSavedLogs / setSavedLogs / clearSavedLogs）：
//     离线或上传失败时暂存日志文本，最多约 3.5KB。
//     下次联网时补传并清空。
//
//   时间与元数据：
//     - predicted_wake_epoch：深睡前推算的下次唤醒时间（离线时间恢复用）
//     - last_water_epoch：上次自动浇水时间戳（防重复浇水）
//     - watering_lock：浇水进行中锁（防 WDT 复位后重复触发）
//     - wdt_streak：连续 WDT 异常计数（退避策略用）
// =============================================================================

#include "nvs_ctrl.h"

#include <Preferences.h>
#include <sys/time.h>
#include <time.h>

namespace {

    static const char *kNamespace = "water_bot";

    // ---- 常驻配置 key ----
    static const char *kCfgSetOn = "cfg_seton";  // settingonline
    static const char *kCfgPSave = "cfg_psave";  // powerSavingMode
    static const char *kCfgStgs  = "cfg_stgs";   // AppSettings 结构体
    static const char *kCfgSRev  = "cfg_srev";   // settings.json rev

    // ---- OTA 临时状态 key ----
    static const char *kOtaFlag  = "ota_flag";   // OTA 恢复标记
    static const char *kWCount   = "w_count";    // wakeupCount 临时值
    static const char *kPSaveTmp = "p_save";     // powerSavingMode 临时值
    static const char *kStgsTmp  = "stgs";       // AppSettings 临时值

    // ---- 日志 & 时间 key ----
    static const char *kSavedLogs      = "saved_logs";   // 日志缓存
    static const char *kPredWakeEpoch  = "t_pred";       // 预计唤醒时间
    static const char *kLastWaterEpoch = "t_wlast";      // 上次浇水时间
    static const char *kWateringLock   = "w_lock";       // 浇水进行中锁
    static const char *kWdtStreak      = "wdt_stk";      // WDT 连续计数
}

namespace NvsCtrl {

// =============================================================================
// 常驻配置 — 跨深睡 & 跨 OTA 保留
// =============================================================================

bool loadConfig(bool &settingonline, bool &powerSavingMode, AppSettings &globalSettings) {
    Preferences prefs;
    prefs.begin(kNamespace, false);

    bool loadedAny = false;
    // isKey 检查：若 NVS 中无此 key（首次启动或 NVS 被清除），保留 RTC 默认值
    if (prefs.isKey(kCfgSetOn)) {
        settingonline = prefs.getBool(kCfgSetOn, settingonline);
        loadedAny = true;
    }
    if (prefs.isKey(kCfgPSave)) {
        powerSavingMode = prefs.getBool(kCfgPSave, powerSavingMode);
        loadedAny = true;
    }
    if (prefs.isKey(kCfgStgs)) {
        prefs.getBytes(kCfgStgs, &globalSettings, sizeof(AppSettings));
        loadedAny = true;
    }

    prefs.end();
    return loadedAny;
}

void saveConfig(bool settingonline, bool powerSavingMode, const AppSettings &globalSettings) {
    Preferences prefs;
    prefs.begin(kNamespace, false);
    prefs.putBool(kCfgSetOn, settingonline);
    prefs.putBool(kCfgPSave, powerSavingMode);
    prefs.putBytes(kCfgStgs, &globalSettings, sizeof(AppSettings));
    prefs.end();
}

uint32_t getSettingsRev() {
    Preferences prefs;
    prefs.begin(kNamespace, false);
    uint32_t rev = prefs.getUInt(kCfgSRev, 0);
    prefs.end();
    return rev;
}

void setSettingsRev(uint32_t rev) {
    Preferences prefs;
    prefs.begin(kNamespace, false);
    prefs.putUInt(kCfgSRev, rev);
    prefs.end();
}

void setSettingOnline(bool enabled) {
    Preferences prefs;
    prefs.begin(kNamespace, false);
    prefs.putBool(kCfgSetOn, enabled);
    prefs.end();
}

// =============================================================================
// OTA 临时状态 — OTA 升级前保存 → 升级后恢复并清除
// =============================================================================

bool hasOtaFlag() {
    Preferences prefs;
    prefs.begin(kNamespace, false);
    bool has = prefs.isKey(kOtaFlag);
    prefs.end();
    return has;
}

void saveOtaState(int wakeupCount, bool powerSavingMode, const AppSettings &globalSettings) {
    Preferences prefs;
    prefs.begin(kNamespace, false);
    prefs.putInt(kWCount, wakeupCount);
    prefs.putBool(kPSaveTmp, powerSavingMode);
    prefs.putBytes(kStgsTmp, &globalSettings, sizeof(AppSettings));
    prefs.putBool(kOtaFlag, true);  // 最后写入标记，确保前面数据已落盘
    prefs.end();
}

bool restoreAfterOta(int &wakeupCount, bool &powerSavingMode, AppSettings &globalSettings) {
    Preferences prefs;
    prefs.begin(kNamespace, false);

    if (!prefs.isKey(kOtaFlag)) {
        prefs.end();
        return false;
    }

    // 恢复临时状态到引用参数
    wakeupCount = prefs.getInt(kWCount, 0);
    powerSavingMode = prefs.getBool(kPSaveTmp, true);
    prefs.getBytes(kStgsTmp, &globalSettings, sizeof(AppSettings));

    // 恢复后立即清除临时 key，避免下次普通启动误触发
    prefs.remove(kOtaFlag);
    prefs.remove(kWCount);
    prefs.remove(kPSaveTmp);
    prefs.remove(kStgsTmp);

    prefs.end();
    return true;
}

void clearOtaFlag() {
    Preferences prefs;
    prefs.begin(kNamespace, false);
    prefs.remove(kOtaFlag);
    prefs.end();
}

// =============================================================================
// 日志缓存 — 离线 / 上传失败时暂存
// =============================================================================

String getSavedLogs() {
    Preferences prefs;
    prefs.begin(kNamespace, false);
    String logs = prefs.getString(kSavedLogs, "");
    prefs.end();
    return logs;
}

void clearSavedLogs() {
    Preferences prefs;
    prefs.begin(kNamespace, false);
    prefs.remove(kSavedLogs);
    prefs.end();
}

bool setSavedLogs(const String &logs) {
    Preferences prefs;
    prefs.begin(kNamespace, false);
    bool ok = prefs.putString(kSavedLogs, logs) > 0;
    prefs.end();
    return ok;
}

// =============================================================================
// 预测唤醒时间 — 离线时恢复相对可用的系统时钟
// =============================================================================

void setPredictedWakeEpoch(uint64_t epochSeconds) {
    Preferences prefs;
    prefs.begin(kNamespace, false);
    prefs.putULong64(kPredWakeEpoch, epochSeconds);
    prefs.end();
}

uint64_t getPredictedWakeEpoch() {
    Preferences prefs;
    prefs.begin(kNamespace, false);
    uint64_t v = prefs.getULong64(kPredWakeEpoch, 0);
    prefs.end();
    return v;
}

static bool isTimeValid(time_t now) {
    const time_t kMinValid = 1577836800; // 2020-01-01
    return now >= kMinValid;
}

bool applyPredictedTimeIfInvalid() {
    time_t now = time(nullptr);
    if (isTimeValid(now)) return false;  // 时间已有效，无需恢复

    uint64_t predicted = getPredictedWakeEpoch();
    if (predicted == 0) return false;    // 无预测值可恢复

    struct timeval tv;
    tv.tv_sec = (time_t)predicted;
    tv.tv_usec = 0;
    if (settimeofday(&tv, nullptr) != 0) {
        return false;
    }

    return isTimeValid(time(nullptr));
}

// =============================================================================
// 浇水安全锁 — 防重复浇水 & WDT 异常复位
// =============================================================================

uint64_t getLastWaterEpoch() {
    Preferences prefs;
    prefs.begin(kNamespace, false);
    uint64_t v = prefs.getULong64(kLastWaterEpoch, 0);
    prefs.end();
    return v;
}

void setLastWaterEpoch(uint64_t epochSeconds) {
    Preferences prefs;
    prefs.begin(kNamespace, false);
    prefs.putULong64(kLastWaterEpoch, epochSeconds);
    prefs.end();
}

void setWateringLock(bool locked) {
    Preferences prefs;
    prefs.begin(kNamespace, false);
    if (locked) {
        prefs.putBool(kWateringLock, true);
    } else {
        prefs.remove(kWateringLock);
    }
    prefs.end();
}

bool getWateringLock() {
    Preferences prefs;
    prefs.begin(kNamespace, false);
    bool locked = prefs.getBool(kWateringLock, false);
    prefs.end();
    return locked;
}

// =============================================================================
// WDT 异常计数 — 用于网络异常时的退避策略
// =============================================================================

uint32_t getWdtStreak() {
    Preferences prefs;
    prefs.begin(kNamespace, false);
    uint32_t v = prefs.getUInt(kWdtStreak, 0);
    prefs.end();
    return v;
}

void setWdtStreak(uint32_t streak) {
    Preferences prefs;
    prefs.begin(kNamespace, false);
    prefs.putUInt(kWdtStreak, streak);
    prefs.end();
}

}
