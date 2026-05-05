// =============================================================================
// LogManager — 日志生成、缓存与云端上传
// =============================================================================
//
// 每次唤醒调用 processLogs() 一次：
//   1. 尝试上传 NVS 中积压的缓存日志 → 成功则清缓存
//   2. 生成本次日志 → 在线则上传，离线则追加到 NVS 缓存
//
// 时间来源优先级：
//   在线：等待 SNTP 同步完成 → 精确到秒
//   离线：使用上次深睡前推算的"预计唤醒时间"（NVS 中） → 可能有数分钟误差
//   都不可用：回退 millis() 作为文件名和时间戳
//
// 日志格式：纯文本，包含时间 / NTP 状态 / 唤醒计数 / 版本 / OTA / 湿度 / 设置
// =============================================================================

#include "log_manager.h"
#include <time.h>
#include <stdlib.h>
#if defined(__has_include)
    #if __has_include(<esp_sntp.h>)
        #include <esp_sntp.h>
        #define HAS_ESP_SNTP 1
    #else
        #define HAS_ESP_SNTP 0
    #endif
#else
    #define HAS_ESP_SNTP 0
#endif
#include "app_config.h"
#include "nvs_ctrl.h"

namespace {

    static constexpr time_t kMinValidEpoch = 1577836800; // 2020-01-01

    // 系统时钟是否指向合理时间（≥2020年）
    static bool isEpochValid() {
        return time(nullptr) >= kMinValidEpoch;
    }

    // 兜底 TZ 设置：避免因未设置时区导致日志显示 UTC 时间
    static void ensureTimeZoneConfigured() {
        const char *tz = getenv("TZ");
        if (tz == nullptr || tz[0] == '\0') {
            setenv("TZ", "CST-8", 1);
            tzset();
        }
    }

    // 等待 SNTP 同步完成（最多 timeoutMs 毫秒）
    static bool waitForSntpSync(uint32_t timeoutMs, void (*keepAlive)()) {
#if !HAS_ESP_SNTP
        (void)timeoutMs;
        (void)keepAlive;
        return false;
#else
        const uint32_t startMs = millis();
        while ((uint32_t)(millis() - startMs) < timeoutMs) {
            if (keepAlive) keepAlive();
            if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
                return true;
            }
            delay(200);
        }
        return false;
#endif
    }

    // 查询当前 SNTP 是否已完成同步
    static bool isNtpSynced() {
#if HAS_ESP_SNTP
        return sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED;
#else
        return false;
#endif
    }
}

// =============================================================================
// 时间同步 — 确保日志时间戳可用
// =============================================================================

bool LogManager::ensureTimeValid(struct tm &timeinfo, bool isOnline, void (*keepAlive)()) {
    ensureTimeZoneConfigured();

    // 离线：不强求 NTP，能用就行（通常来自 NVS 预测时间）
    if (!isOnline) {
        return (getLocalTime(&timeinfo) && isEpochValid());
    }

    // 在线：触发 NTP 同步并等待完成
    const char *tz = getenv("TZ");
    if (tz == nullptr || tz[0] == '\0') {
        tz = "CST-8";
    }
    configTzTime(tz, "ntp.aliyun.com", "pool.ntp.org");

    (void)waitForSntpSync(12000, keepAlive);

    return (getLocalTime(&timeinfo) && isEpochValid());
}

void LogManager::begin() {
    // 预留：未来如需预分配缓冲区或初始化统计信息，在此扩展
}

// =============================================================================
// 日志处理主入口 — 每次唤醒调用一次
// =============================================================================

void LogManager::processLogs(GitCtrl& git,
                             int wakeupCount,
                             bool isUpdateAvailable,
                             String newVersion,
                             int moisturePercent,
                             bool isOnline,
                             bool otaChecked,
                             bool settingonline,
                             bool powerSavingMode,
                             const AppSettings &globalSettings,
                             void (*keepAlive)()) {
    struct tm timeinfo;
    bool timeValid = ensureTimeValid(timeinfo, isOnline, keepAlive);
    const bool ntpSynced = isOnline ? isNtpSynced() : false;

    // 格式化时间字符串（用于日志内容和文件名）
    char timeStr[64];
    if (timeValid) {
        strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);
    } else {
        sprintf(timeStr, "Time Unknown (millis: %lu)", millis());
    }

    // ---- 构建本次日志内容 ----

    String logContent = "Device Report:\n";
    logContent += "Time: " + String(timeStr) + "\n";
    logContent += "NTP: " + String(ntpSynced ? "TRUE" : "FALSE") + "\n";
    logContent += "Wakeup Count: " + String(wakeupCount) + "\n";
    logContent += "Current Version: " + String(CURRENT_VERSION) + "\n";
    logContent += "OTA Checked: " + String(otaChecked ? "Yes" : "No") + "\n";
    logContent += isUpdateAvailable ? ("Status: Update found " + newVersion + "\n") : "Status: No Update\n";
    logContent += "Moisture: " + String(moisturePercent) + "%\n";

    // 当前设备设置快照
    uint32_t settingsRev = NvsCtrl::getSettingsRev();
    logContent += "Settings:\n";
    logContent += "  settingonline: " + String(settingonline ? "true" : "false") + "\n";
    logContent += "  powerSavingMode: " + String(powerSavingMode ? "true" : "false") + "\n";
    logContent += "  waterTimeSeconds: " + String(globalSettings.waterTimeSeconds) + "\n";
    logContent += "  sleepHours: " + String(globalSettings.sleepHours) + "\n";
    logContent += "  sleepMinutes: " + String(globalSettings.sleepMinutes) + "\n";
    logContent += "  settings_rev: " + String((unsigned long)settingsRev) + "\n";

    // ---- 第 1 步：补传 NVS 中积压的缓存日志 ----

    String cachedLogs = NvsCtrl::getSavedLogs();

    if (cachedLogs.length() > 0) {
        String cacheFilename;
        if (timeValid) {
            char tmp[64];
            strftime(tmp, sizeof(tmp), "logs/%Y%m%d_%H%M%S_supp.txt", &timeinfo);
            cacheFilename = String(tmp);
        } else {
            cacheFilename = "logs/log_cached" + String(millis()) + ".txt";
        }

        String uploadContent = cachedLogs + "\n\n[Supplementary Log]";

        bool uploadSuccess = false;
        if (isOnline) {
            Serial.println("[Log] Found cached logs, attempting upload...");
            uploadSuccess = git.uploadTextFile(cacheFilename, uploadContent, "Daily Log (Supplementary)", "master", keepAlive);
        }

        if (uploadSuccess) {
            Serial.println("[Log] Cached logs uploaded. Clearing NVS cache.");
            NvsCtrl::clearSavedLogs();
            cachedLogs = "";
            if (keepAlive) keepAlive();
        } else {
            Serial.println("[Log] Cached logs retention (Offline or Failed).");
        }
    }

    // ---- 第 2 步：上传本次日志（在线）或缓存（离线） ----

    String currentFileName;
    if (timeValid) {
        char tmp[64];
        strftime(tmp, sizeof(tmp), "logs/%Y%m%d_%H%M%S_cur.txt", &timeinfo);
        currentFileName = String(tmp);
    } else {
        currentFileName = "logs/log_" + String(millis()) + ".txt";
    }

    bool currentUploadSuccess = false;
    if (isOnline) {
        currentUploadSuccess = git.uploadTextFile(currentFileName, logContent, "Daily Log " + String(timeStr), "master", keepAlive);
    }

    if (currentUploadSuccess) {
        Serial.println("[Log] Current log uploaded successfully.");
    } else {
        if (!isOnline) Serial.println("[Log] Offline mode. Saving to NVS cache...");
        else Serial.println("[Log] Upload failed. Saving to NVS cache...");

        // 追加到 NVS 缓存（与已有缓存用分隔线隔开）
        if (cachedLogs.length() > 0) {
             cachedLogs += "\n--------------------------------\n";
        }
        cachedLogs += "OriginalFile: " + currentFileName + "\n" + logContent;

        // 缓存上限约 3.5KB，超出则丢弃本次日志（优先保留旧缓存）
        if (cachedLogs.length() < 3500) {
             NvsCtrl::setSavedLogs(cachedLogs);
        } else {
             Serial.println("[Log] Cache full! Discarding new log.");
        }
    }
}
