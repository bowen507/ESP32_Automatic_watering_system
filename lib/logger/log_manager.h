#ifndef LOG_MANAGER_H
#define LOG_MANAGER_H
#include <Arduino.h>
#include <time.h>
#include "git_ctrl.h"
#include "app_config.h"

class LogManager {
public:
    /**
     * @brief 日志管理器初始化。
     *
     * 当前实现中保留为占位接口：如果未来需要预分配缓冲区、初始化统计信息、或建立日志队列，
     * 可在此处完成。
     */
    void begin();

    /**
     * @brief 处理一次“维护日志”流程：构建当前日志 -> 补传缓存日志（若有）-> 上传当前日志 -> 失败则缓存。
     *
     * - 离线时不做网络操作，只把日志追加写入 NVS 缓存。
     * - 在线时优先补传缓存日志，成功后清空缓存，再上传本次日志。
     *
     * 时间来源：
     * - 在线时会尽力确保 timeinfo 有效（可能触发 NTP 同步与重试），用于生成稳定的文件名与日志时间戳。
     * - 时间不可用时，回退为使用 millis() 进行标记与文件命名。
     *
     * 注意：
     * - 本函数可能包含短暂阻塞（例如 NTP 重试、为避免文件名冲突的 delay）。
     * - keepAlive 回调用于在阻塞期间维持上层网络心跳（例如 Blinker.run）。
     *
     * @param git               GitCtrl 实例（用于上传文本文件）
     * @param wakeupCount       本次唤醒计数（写入日志内容）
     * @param isUpdateAvailable 是否检测到 OTA 更新（写入日志内容）
     * @param newVersion        新版本号（若有，写入日志内容）
     * @param moisturePercent   当前湿度百分比（写入日志内容）
     * @param isOnline          本次是否在线（决定上传还是缓存）
     * @param keepAlive         可选：阻塞期间的心跳回调（例如 Blinker.run）；离线可传 nullptr
     */
    void processLogs(GitCtrl& git,
                     int wakeupCount,
                     bool isUpdateAvailable,
                     String newVersion,
                     int moisturePercent,
                     bool isOnline,
                     bool otaChecked,
                     bool settingonline,
                     bool powerSavingMode,
                     const AppSettings &globalSettings,
                     void (*keepAlive)() = nullptr);
private:
    /**
     * @brief 尝试确保 timeinfo 可用。
     *
     * - 若离线：直接返回 false。
     * - 若在线：优先尝试 getLocalTime（若 NetworkManager 已完成 NTP，同步应当很快成功）。
     * - 若失败：执行 configTime 并进行最多若干次重试。
     *
     * @param[out] timeinfo   成功时填充的本地时间结构体
     * @param isOnline        是否在线
     * @param keepAlive       重试等待期间可选心跳回调
     * @return bool           成功获取有效时间返回 true，否则 false。
     */
    bool ensureTimeValid(struct tm &timeinfo, bool isOnline, void (*keepAlive)());
};

#endif