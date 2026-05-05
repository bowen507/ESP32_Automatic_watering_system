// NVS controller for water_bot

#ifndef NVS_CTRL_H
#define NVS_CTRL_H

#include <Arduino.h>
#include "app_config.h"

namespace NvsCtrl {

/**
 * @brief 从 NVS（Preferences）加载常驻配置。
 *
 * 该函数用于在设备启动时恢复“跨深睡 + 跨 OTA 复位”都需要保留的配置项。
 * 读取时会先判断 key 是否存在（isKey），不存在则保持传入的默认值不变。
 *
 * 常驻配置当前包括：
 * - settingonline：远程设置总开关（用于决定是否拉取 settings.json）
 * - powerSavingMode：节电模式开关
 * - globalSettings：浇水与休眠参数（结构体）
 *
 * @param[in,out] settingonline  远程设置总开关（若 NVS 中存在则覆盖）
 * @param[in,out] powerSavingMode 节电模式（若 NVS 中存在则覆盖）
 * @param[in,out] globalSettings  应用设置结构体（若 NVS 中存在则覆盖）
 * @return bool 若至少成功加载了一个配置键则返回 true；否则返回 false。
 */
bool loadConfig(bool &settingonline, bool &powerSavingMode, AppSettings &globalSettings);

/**
 * @brief 将常驻配置写入 NVS（Preferences）。
 *
 * 用途：
 * - 用户在本地菜单修改参数后保存
 * - 远程 settings.json 下发配置后保存
 *
 * 注意：该写入是“覆盖式”的，会更新对应 key 的值。
 *
 * @param settingonline   远程设置总开关
 * @param powerSavingMode 节电模式开关
 * @param globalSettings  应用设置结构体
 */
void saveConfig(bool settingonline, bool powerSavingMode, const AppSettings &globalSettings);

/**
 * @brief 获取已应用的 settings.json 修订号（rev）。
 *
 * 用途：避免每次联网都重复应用相同的远程配置。
 * 当远程 rev 递增时才重新应用 settings.json（若项目逻辑启用 rev）。
 *
 * @return uint32_t 已保存的 rev；若不存在则返回 0。
 */
uint32_t getSettingsRev();

/**
 * @brief 保存/更新已应用的 settings.json 修订号（rev）。
 *
 * @param rev 要写入的修订号。
 */
void setSettingsRev(uint32_t rev);

/**
 * @brief 仅更新远程设置总开关 settingonline 到 NVS。
 *
 * 用途：当 version.json 控制总开关发生变化时，立即落盘，确保下次离线唤醒也能生效。
 *
 * @param enabled 新的开关值。
 */
void setSettingOnline(bool enabled);

/**
 * @brief 检查是否存在 OTA 临时标记（ota_flag）。
 *
 * OTA 更新成功会触发软件重启，RTC_DATA_ATTR 变量会丢失。
 * 项目通过在执行 OTA 前写入 ota_flag 及关键状态，重启后恢复并清除标记。
 *
 * @return bool 若检测到 ota_flag 则返回 true。
 */
bool hasOtaFlag();

/**
 * @brief 在执行 OTA 前保存“临时状态”到 NVS，并写入 ota_flag。
 *
 * 建议在调用 gitCtrl.performUpdate(...) 之前调用本函数。
 * 保存内容为“为了 OTA 后恢复运行所必需的状态”，通常包括 wakeupCount、节电模式与 AppSettings。
 *
 * @param wakeupCount      唤醒计数（将保存为临时 key）
 * @param powerSavingMode  节电模式（将保存为临时 key）
 * @param globalSettings   应用设置结构体（将保存为临时 key）
 */
void saveOtaState(int wakeupCount, bool powerSavingMode, const AppSettings &globalSettings);

/**
 * @brief OTA 后恢复临时状态，并清除临时 key 与 ota_flag。
 *
 * 典型调用时机：setup() 早期。
 * 若 ota_flag 存在，则从 NVS 取回保存的状态覆盖传入引用参数，并清除临时键，避免下次普通启动误触发恢复。
 *
 * @param[in,out] wakeupCount      唤醒计数（被恢复值覆盖）
 * @param[in,out] powerSavingMode  节电模式（被恢复值覆盖）
 * @param[in,out] globalSettings   应用设置结构体（被恢复值覆盖）
 * @return bool 若进行了恢复则返回 true；否则返回 false。
 */
bool restoreAfterOta(int &wakeupCount, bool &powerSavingMode, AppSettings &globalSettings);

/**
 * @brief 仅清除 ota_flag 标记。
 *
 * 用途：当 OTA 失败（performUpdate 返回失败）时，避免下次启动误判为“刚 OTA 完成”。
 * 注意：本函数仅移除 ota_flag，不会清除临时状态数据；只要 ota_flag 不存在，就不会触发恢复。
 */
void clearOtaFlag();

/**
 * @brief 获取 NVS 中缓存的日志字符串。
 *
 * 日志缓存用于：离线或上传失败时暂存日志内容，待下次联网后补传。
 *
 * @return String 缓存内容；若不存在则返回空字符串。
 */
String getSavedLogs();

/**
 * @brief 清除 NVS 中缓存的日志字符串。
 */
void clearSavedLogs();

/**
 * @brief 写入/覆盖 NVS 中缓存的日志字符串。
 *
 * @param logs 要缓存的日志内容。
 * @return bool 写入成功返回 true；失败返回 false。
 */
bool setSavedLogs(const String &logs);

/**
 * @brief 保存“预计唤醒时间”的 Epoch 秒（用于离线唤醒时恢复一个可用时间）。
 *
 * 典型调用：进入深睡前，用当前 time(nullptr) + sleepSeconds 计算出下一次唤醒的预计时间并写入。
 */
void setPredictedWakeEpoch(uint64_t epochSeconds);

/**
 * @brief 读取已保存的“预计唤醒时间”Epoch 秒。
 * @return uint64_t 若不存在返回 0。
 */
uint64_t getPredictedWakeEpoch();

/**
 * @brief 若系统时间无效且存在预计唤醒时间，则用它设置系统时钟。
 *
 * - 离线唤醒时：可让日志/显示使用一个“相对可用”的时间。
 * - 在线唤醒时：随后仍会 NTP 同步并覆盖。
 * - 时区（TZ）应由应用层统一配置（例如 main.cpp 中 setenv/tzset）；本函数不负责设置 TZ。
 *
 * @return bool 若成功应用返回 true。
 */
bool applyPredictedTimeIfInvalid();

/**
 * @brief 获取上次自动浇水发生的 Epoch 秒（用于跨重启/跨 OTA 的防重复浇水）。
 * @return uint64_t 若不存在返回 0。
 */
uint64_t getLastWaterEpoch();

/**
 * @brief 记录上次自动浇水发生的 Epoch 秒。
 */
void setLastWaterEpoch(uint64_t epochSeconds);

/**
 * @brief 设置/清除“浇水进行中”锁。
 *
 * 用途：如果设备在水泵开启期间因 WDT/异常复位，下一次启动能检测到锁并跳过浇水，避免重复触发。
 */
void setWateringLock(bool locked);

/**
 * @brief 读取“浇水进行中”锁。
 */
bool getWateringLock();

/**
 * @brief 连续 WDT 异常重启计数（用于网络异常时的退避策略）。
 */
uint32_t getWdtStreak();
void setWdtStreak(uint32_t streak);

}

#endif
