// =============================================================================
// NetworkManager — 网络连接、NTP 时间同步、远程设置拉取、深睡管理
// =============================================================================
//
// 职责划分：
//   - 联网：WiFi 连接 + NTP 时间同步（begin / beginByPolicy）
//   - 远程设置：version.json → settings.json 的拉取与应用（applyOnlineSettingsIfEnabled）
//   - 深度睡眠：断开所有连接后进入定时深睡（enterDeepSleep）
//   - WiFi 恢复：OTA 失败后重置 TLS/WiFi 栈（resetWiFi）
//
// 节电联网策略（beginByPolicy）：
//   节电模式：偶数次唤醒（0, 2, 4...）联网，奇数次离线
//   普通模式：始终联网
// =============================================================================

#include "network_manager.h"
#include <esp_wifi.h>
#include <WiFi.h>
#include <time.h>
#if defined(__has_include)
  #if __has_include(<esp_sntp.h>)
    #include <esp_sntp.h>
  #elif __has_include(<lwip/apps/sntp.h>)
    #include <lwip/apps/sntp.h>
  #endif
#endif
#include <ArduinoJson.h>
#include "nvs_ctrl.h"

// =============================================================================
// 联网策略
// =============================================================================

bool NetworkManager::beginByPolicy(bool powerSavingMode, int wakeupCount, bool &shouldConnectOut) {
    shouldConnectOut = !powerSavingMode || (wakeupCount % 2 == 0);

    if (!shouldConnectOut) {
        Serial.println("[System] Offline Wakeup (Power Saving)");
        return false;
    }

    Serial.println("[System] Online Wakeup");
    return begin();
}

// =============================================================================
// 远程设置控制 — version.json / settings.json
// =============================================================================

bool NetworkManager::fetchSettingsControlFromVersionJson(GitCtrl &git,
                                                        bool shouldConnect,
                                                        const char *versionUrl,
                                                        bool currentEnabled,
                                                        const char *defaultSettingsUrl,
                                                        bool &enabledOut,
                                                        String &settingsUrlOut,
                                                        uint32_t &revOut) {
    enabledOut = currentEnabled;
    settingsUrlOut = String(defaultSettingsUrl);
    settingsUrlOut.trim();
    revOut = 0;

    if (!shouldConnect) return false;

    String payload;
    if (!git.downloadText(String(versionUrl), payload)) {
        Serial.println("[Settings] download version.json failed (use local settingonline/default url)");
        return false;
    }

    DynamicJsonDocument doc(2048);
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
        Serial.printf("[Settings] version.json parse error: %s\n", err.c_str());
        return false;
    }

    // 解析远程设置总开关（兼容 settingonline / settings_enabled 两种字段名）
    if (doc["settingonline"].is<bool>()) {
        enabledOut = doc["settingonline"].as<bool>();
    } else if (doc["settings_enabled"].is<bool>()) {
        enabledOut = doc["settings_enabled"].as<bool>();
    }

    // 解析 settings.json 的 URL（可覆盖默认地址）
    if (doc["settings_url"].is<const char*>()) {
        settingsUrlOut = String(doc["settings_url"].as<const char*>());
        settingsUrlOut.trim();
    }

    // 解析修订号（用于跳过重复下载）
    if (doc["settings_rev"].is<uint32_t>()) {
        revOut = doc["settings_rev"].as<uint32_t>();
    }

    return true;
}

bool NetworkManager::applyOnlineSettingsIfEnabled(GitCtrl &git,
                                                 bool shouldConnect,
                                                 const char *versionUrl,
                                                 const char *defaultSettingsUrl,
                                                 bool &settingonline,
                                                 bool &powerSavingMode,
                                                 AppSettings &globalSettings) {
    if (!shouldConnect) return false;

    bool enabled = settingonline;
    String settingsUrl;
    uint32_t settingsRevFromVersion = 0;
    fetchSettingsControlFromVersionJson(git, shouldConnect, versionUrl, settingonline, defaultSettingsUrl,
                                       enabled, settingsUrl, settingsRevFromVersion);

    // version.json 可能动态关闭远程设置
    if (enabled != settingonline) {
        settingonline = enabled;
        NvsCtrl::setSettingOnline(settingonline);
    }

    if (!settingonline) {
        Serial.println("[Settings] settingonline=false (from version.json), skip remote settings");
        return false;
    }

    // 修订号检查：version.json 已提供 rev 且不比本地新，跳过下载
    uint32_t lastRev = NvsCtrl::getSettingsRev();
    if (settingsRevFromVersion != 0 && settingsRevFromVersion <= lastRev) {
        Serial.printf("[Settings] No new settings (rev=%u, last=%u)\n",
                      (unsigned)settingsRevFromVersion, (unsigned)lastRev);
        return false;
    }

    String payload;
    if (!git.downloadText(settingsUrl, payload)) {
        Serial.print("[Settings] download settings json failed: ");
        Serial.println(settingsUrl);
        return false;
    }

    DynamicJsonDocument doc(2048);
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
        Serial.printf("[Settings] settings json parse error: %s\n", err.c_str());
        return false;
    }

    // 若 version.json 未提供 rev，用 settings.json 自身的 rev 兜底
    lastRev = NvsCtrl::getSettingsRev();

    uint32_t rev = settingsRevFromVersion;
    if (rev == 0) {
        if (doc["rev"].is<uint32_t>()) rev = doc["rev"].as<uint32_t>();
        else if (doc["settings_rev"].is<uint32_t>()) rev = doc["settings_rev"].as<uint32_t>();
    }

    if (rev != 0 && rev <= lastRev) {
        Serial.printf("[Settings] No new settings (rev=%u, last=%u)\n", (unsigned)rev, (unsigned)lastRev);
        return false;
    }

    // 解析 settings.json 中的各项参数
    bool changed = false;

    if (doc["powerSavingMode"].is<bool>()) {
        bool newPowerSavingMode = doc["powerSavingMode"].as<bool>();
        if (newPowerSavingMode != powerSavingMode) {
            powerSavingMode = newPowerSavingMode;
            changed = true;
        }
    }

    // 兼容两种 JSON 结构：顶层字段 与 globalSettings 子对象
    JsonVariantConst gs = doc["globalSettings"];
    int wt = gs.isNull() ? (doc["waterTimeSeconds"].is<int>() ? doc["waterTimeSeconds"].as<int>() : globalSettings.waterTimeSeconds)
                         : (gs["waterTimeSeconds"].is<int>() ? gs["waterTimeSeconds"].as<int>() : globalSettings.waterTimeSeconds);
    int sh = gs.isNull() ? (doc["sleepHours"].is<int>() ? doc["sleepHours"].as<int>() : globalSettings.sleepHours)
                         : (gs["sleepHours"].is<int>() ? gs["sleepHours"].as<int>() : globalSettings.sleepHours);
    int sm = gs.isNull() ? (doc["sleepMinutes"].is<int>() ? doc["sleepMinutes"].as<int>() : globalSettings.sleepMinutes)
                         : (gs["sleepMinutes"].is<int>() ? gs["sleepMinutes"].as<int>() : globalSettings.sleepMinutes);

    // 参数合法性校验
    if (wt < 1) wt = 1;
    if (wt > 300) wt = 300;
    if (sh < 0) sh = 0;
    if (sh > 23) sh = 23;
    if (sm < 0) sm = 0;
    if (sm > 59) sm = 59;

    if (wt != globalSettings.waterTimeSeconds || sh != globalSettings.sleepHours || sm != globalSettings.sleepMinutes) {
        globalSettings.waterTimeSeconds = wt;
        globalSettings.sleepHours = sh;
        globalSettings.sleepMinutes = sm;
        changed = true;
    }

    if (!changed) {
        Serial.println("[Settings] settings.json applied: no changes");
        if (rev != 0 && rev > lastRev) {
            NvsCtrl::setSettingsRev(rev);
        }
        return false;
    }

    // 变更写入 NVS，确保跨深睡 & 跨 OTA 后仍生效
    if (rev != 0 && rev > lastRev) NvsCtrl::setSettingsRev(rev);
    NvsCtrl::saveConfig(settingonline, powerSavingMode, globalSettings);

    Serial.println("[Settings] Remote settings applied & saved to NVS:");
    Serial.printf("[Settings] settingonline=%s, powerSavingMode=%s, waterTime=%ds, sleep=%dh%dm\n",
                  settingonline ? "true" : "false",
                  powerSavingMode ? "true" : "false",
                  globalSettings.waterTimeSeconds,
                  globalSettings.sleepHours,
                  globalSettings.sleepMinutes);

    return true;
}

// =============================================================================
// 构造与初始化
// =============================================================================

NetworkManager::NetworkManager(const char* blinkerAuth, const char* ssid, const char* password, const uint8_t* mac) {
    _auth = blinkerAuth;
    _ssid = ssid;
    _password = password;
    _stationMAC = mac;
}

bool NetworkManager::begin() {
    Serial.println("\n[NetworkManager] Starting network initialization...");

    if (!connectToWiFi()) {
        Serial.println("[NetworkManager] Failed to connect to WiFi.");
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        return false;
    }

    // NTP 同步失败不阻断整体流程：离线时可依赖 NVS 预测时间兜底
    if (!syncNTPTime()) {
        Serial.println("[NetworkManager] Failed to sync NTP time. Continuing anyway...");
    }

    Serial.println("[NetworkManager] All network services initialized successfully.");
    return true;
}

// =============================================================================
// WiFi 连接
// =============================================================================

bool NetworkManager::connectToWiFi() {
    // 清理残留连接状态（如软件复位后）
    WiFi.disconnect(true);
    WiFi.mode(WIFI_STA);

    // 设置自定义 MAC 地址（需在 WiFi.begin 之前）
    if (_stationMAC != nullptr) {
        esp_err_t err = esp_wifi_set_mac(WIFI_IF_STA, _stationMAC);
        if (err != ESP_OK) {
            Serial.printf("[NetworkManager] esp_wifi_set_mac failed: %d (continue)\n", (int)err);
        }
    }
    Serial.print("[NetworkManager] Connecting to WiFi: ");
    Serial.println(_ssid);

    // persistent(false): 不把 WiFi 凭据写入 NVS，减少 Flash 磨损
    // setAutoReconnect(true): 允许底层自动重连
    WiFi.persistent(false);
    WiFi.setAutoReconnect(true);

    WiFi.begin(_ssid, _password);

    const uint32_t kTimeoutMs = 20000;          // 总超时 20 秒
    const uint32_t kRestartIntervalMs = 15000;  // 15 秒无连接则重启配网流程
    uint32_t startMs = millis();
    uint32_t lastDotMs = 0;
    uint32_t lastRestartMs = startMs;

    while (WiFi.status() != WL_CONNECTED) {
        delay(200);
        if (millis() - lastDotMs >= 1000) {
            Serial.print(".");
            lastDotMs = millis();
        }

        // 长时间未连上时重启连接流程（某些路由/信道下可提高成功率）
        if (millis() - lastRestartMs >= kRestartIntervalMs) {
            wl_status_t st = WiFi.status();
            Serial.printf("\n[NetworkManager] WiFi still not connected (status=%d), restarting...\n", (int)st);
            WiFi.disconnect(true);
            delay(50);
            WiFi.begin(_ssid, _password);
            lastRestartMs = millis();
        }

        if (millis() - startMs >= kTimeoutMs) {
            Serial.println("\n[NetworkManager] WiFi connection timeout.");
            WiFi.disconnect(true);
            WiFi.mode(WIFI_OFF);
            return false;
        }
    }
    Serial.println("\n[NetworkManager] WiFi connected.");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
    Serial.printf("Heap: %d, MAC: %s\n",
                   esp_get_free_heap_size(),
                   WiFi.macAddress().c_str());
    return true;
}

// =============================================================================
// NTP 时间同步
// =============================================================================

bool NetworkManager::syncNTPTime() {
    // 使用 configTzTime（而非 configTime）：
    // configTime(gmtOffset, ...) 会通过 setenv 修改全局 TZ，传 0 会把时区改成 UTC0
    // configTzTime 按当前 TZ 同步，保持东八区不变
    const char *tz = getenv("TZ");
    if (tz == nullptr || tz[0] == '\0') {
        tz = "CST-8";
    }
    configTzTime(tz, "ntp.aliyun.com", "pool.ntp.org");
    Serial.println("[NetworkManager] Waiting for NTP time sync...");

    struct tm timeinfo;

    // 优先等待 SNTP 状态变为 COMPLETED，而不是仅依赖 getLocalTime（后者在
    // 已有 RTC 时间但未完成 NTP 时也可能返回 true）
    const uint32_t startMs = millis();
    const uint32_t kTimeoutMs = 12000;
    while ((uint32_t)(millis() - startMs) < kTimeoutMs) {
        if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) break;
        delay(200);
    }

    if (!getLocalTime(&timeinfo, 5000)) {
        Serial.println("\n[NetworkManager] NTP sync timeout.");
        return false;
    }

    // 二次校验：排除预存时间模拟的假同步
    const time_t now = time(nullptr);
    const time_t kMinValid = 1577836800; // 2020-01-01 00:00:00 UTC
    if (now < kMinValid) {
        Serial.println("\n[NetworkManager] NTP time invalid after sync (continue anyway)");
        return false;
    }

    Serial.println("\n[NetworkManager] NTP time synced.");
    return true;
}

// =============================================================================
// 时间工具
// =============================================================================

String NetworkManager::getCurrentTime() {
    time_t now = time(nullptr);
    struct tm *timeinfo = localtime(&now);
    char buffer[32];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", timeinfo);
    return String(buffer);
}

// =============================================================================
// WiFi 栈重置 — OTA 失败后清理 TLS/WiFi 脏状态
// =============================================================================

bool NetworkManager::resetWiFi() {
    Serial.println("[NetworkManager] Resetting WiFi stack...");
    WiFi.disconnect(true);
    delay(100);
    WiFi.mode(WIFI_OFF);   // 完全关闭 WiFi 以清除底层状态
    delay(50);
    return connectToWiFi(); // 重新连接
}

// =============================================================================
// 深度睡眠
// =============================================================================

void NetworkManager::enterDeepSleep(uint64_t microseconds) {
    digitalWrite(PUMP_PIN, LOW);        // 确保水泵关闭
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    esp_sleep_enable_timer_wakeup(microseconds);
    Serial.println("[NetworkManager] Entering deep sleep...");
    Serial.flush();
    esp_deep_sleep_start();
}
