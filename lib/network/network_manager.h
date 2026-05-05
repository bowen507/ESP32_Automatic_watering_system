#ifndef NETWORK_MANAGER_H
#define NETWORK_MANAGER_H

#define BLINKER_WIFI
#include <Arduino.h>
#include "pins_config.h" // 包含引脚定义
#include "function_declarations.h"

#include "app_config.h"
#include "git_ctrl.h"

// 前向声明，避免循环包含（历史遗留：当前 NetworkManager 内部并未直接使用这些类型）
class BlinkerNumber;
class BlinkerText;
class BlinkerButton;

/**
 * @brief 网络与时间同步管理器。
 *
 * 设计目标：
 * - 深睡驱动项目：在 setup() 内完成一次“按需联网 → 同步时间 → 拉取远程配置(可选)”后退出。
 * - 省电：联网是最大耗电项，因此提供 beginByPolicy() 统一计算本次是否联网。
 * - 兼容策略：
 *   - WiFi 连接失败：返回 false，调用方应当按离线流程继续（并在实现中尽量关闭 WiFi 以省电）。
 *   - NTP 同步失败：不会阻断整体 begin()（返回 true），仅打印告警；离线时可以依赖 NVS 预测时间兜底。
 *   - 远程 settings/version 拉取失败：保持本地/NVS 配置不变，不影响本次其它流程。
 */
class NetworkManager {
  public:
    /**
     * @brief 构造函数（仅保存必要的连接信息）。
     *
     * 注意：
     * - station MAC 使用指针传入，调用方需保证该内存在整个运行期间有效（通常为全局数组）。
     */
    NetworkManager(const char* blinkerAuth, const char* ssid, const char* password, const uint8_t* mac);

    /**
     * @brief 初始化网络基础能力（WiFi + NTP）。
     *
     * 语义：
     * - 返回 true：WiFi 已连接；NTP 同步“尝试过”，即使失败也不会导致返回 false。
     * - 返回 false：WiFi 未能连接成功。
     *
     * 省电提示：
     * - 调用方若决定离线运行，应避免后续触发网络请求（HTTP/TLS）。
     */
    bool begin();

    /**
     * @brief 按“节电策略 + 唤醒计数”决定本次是否联网，并在需要时调用 begin()。
     *
     * @param powerSavingMode 是否节电模式。
     * @param wakeupCount     当前唤醒计数（由调用方维护，通常是 RTC_DATA_ATTR 变量）。
     * @param[out] shouldConnectOut 策略层面是否应该联网（即使应该联网，也可能因为 WiFi 失败而不在线）。
     *
     * @return bool 实际是否在线：true 表示 WiFi 已连接且基础网络可用；false 表示本次离线（策略离线或联网失败）。
     */
    bool beginByPolicy(bool powerSavingMode, int wakeupCount, bool &shouldConnectOut);

    /**
     * @brief 若启用远程设置，则按 version.json 控制信息拉取并应用 settings.json。
     *
     * 工作流程：
     * 1) 读取 version.json（如果可用），解析：
     *    - settingonline/settings_enabled：远程设置总开关
     *    - settings_url：settings.json 地址（可覆盖默认 URL）
     *    - settings_rev：修订号（用于避免重复下载/应用）
     * 2) 若远程设置开关为 true，则下载 settings.json 并应用 powerSavingMode/globalSettings
     * 3) 将变更与 rev 写入 NVS（跨深睡 & 跨 OTA 生效）
     *
     * 兼容策略：
     * - shouldConnect=false：直接返回 false，不进行任何网络请求。
     * - 下载/解析失败：返回 false，保持原配置不变。
     * - rev 未更新：返回 false（不重复应用）。
     *
     * @param git GitCtrl 实例（用于下载文本）。
     * @param shouldConnect 本次是否在线（通常传入 begin()/beginByPolicy() 的返回值）。
     * @param versionUrl version.json 的 raw URL。
     * @param defaultSettingsUrl 默认 settings.json URL（若 version.json 未提供 settings_url 则使用它）。
     * @param[in,out] settingonline 远程设置总开关（可能被 version.json 覆盖并落盘至 NVS）。
     * @param[in,out] powerSavingMode 节电模式（可能被 settings.json 覆盖并落盘至 NVS）。
     * @param[in,out] globalSettings 全局设置（可能被 settings.json 覆盖并落盘至 NVS）。
     *
     * @return bool true 表示本次确实应用了 settings.json 并写入 NVS；false 表示未应用（离线/失败/无更新/无变更）。
     */
    bool applyOnlineSettingsIfEnabled(GitCtrl &git,
                     bool shouldConnect,
                     const char *versionUrl,
                     const char *defaultSettingsUrl,
                     bool &settingonline,
                     bool &powerSavingMode,
                     AppSettings &globalSettings);

    /**
     * @brief 获取当前本地时间字符串（基于 localtime）。
     *
     * 说明：
     * - 若 NTP 同步成功或 NVS 预测时间已应用，则该时间可用于日志/显示。
     * - 若系统时间仍无效（例如首次离线启动且未保存预测时间），返回值可能落在 1970 年附近。
     */
    String getCurrentTime();

    /**
     * @brief OTA 失败后重置 WiFi 栈，清除残留的 TLS/WiFi 脏状态。
     *
     * 说明：
     * - 内部执行 WiFi.disconnect + 短暂延迟 + 重新连接。
     * - 仅当本次唤醒在线时才调用，避免离线触发不必要的 WiFi 操作。
     *
     * @return bool 重连成功返回 true，失败返回 false。
     */
    bool resetWiFi();

    /**
     * @brief 断开网络并进入深度睡眠。
     *
     * @param microseconds 深睡时长（微秒）。
     *
     * 安全性：
     * - 内部会强制将 PUMP_PIN 置 LOW，避免深睡期间水泵误开启。
     */
    void enterDeepSleep(uint64_t microseconds);


  private:
    // 私有成员变量，存储配置信息
    const char* _auth;
    const char* _ssid;
    const char* _password;
    const uint8_t* _stationMAC; // 使用指针，避免数组拷贝

    // 私有成员函数：内部实现各步骤
    bool connectToWiFi();
    bool syncNTPTime();

    bool fetchSettingsControlFromVersionJson(GitCtrl &git,
                        bool shouldConnect,
                        const char *versionUrl,
                        bool currentEnabled,
                        const char *defaultSettingsUrl,
                        bool &enabledOut,
                        String &settingsUrlOut,
                        uint32_t &revOut);
};


#endif