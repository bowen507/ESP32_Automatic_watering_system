#ifndef MENU_MANAGER_H
#define MENU_MANAGER_H

#include "oled_ctrl.h"
#include "pins_config.h" // 包含按键引脚定义
#include "app_config.h"


// 菜单枚举
enum MenuState {
    MENU_DASHBOARD,     // 仪表盘（实时数据）
    MENU_MAIN,          // 主菜单列表
    MENU_SETTINGS,      // 设置菜单
    MENU_WATER_TIME,    // 浇水时长选择
    MENU_SLEEP_TIME,    // 休眠设置主菜单
    MENU_SLEEP_HOURS,   // 休眠小时选择
    MENU_SLEEP_MINUTES, // 休眠分钟选择
    MENU_WAKEUP_COUNT,  // 唤醒计数控制
    MENU_MOISTURE_SET,  // 湿度设置
    MENU_PUMP_CONTROL,  // 水泵控制
    MENU_INFO,          // 系统信息
    MENU_POWER_MODE,    // 节电模式设置
    MENU_SCREEN_OFF     // 息屏状态
};

class MenuManager {
private:
    DisplayManager* oled;
    MenuState currentMenu;
    AppSettings* settings; // 指向全局设置的指针
    int* wakeupCountMenuPtr; // 指向唤醒计数的指针
    bool* powerModePtr;      // 指向节电模式变量的指针

    int menuIndex;
    bool lastUpState, lastSelectState, lastDownState;
    unsigned long lastDebounceTime;
    unsigned long lastButtonCheckTime; // 新增：上次检查按键的时间
    bool isDisplayOn; // 显示屏状态

    bool settingsDirty; // 菜单中是否修改过配置（用于触发外部持久化）

    // 菜单数据
    static const int MAIN_MENU_ITEMS = 3;
    String mainMenuItems[MAIN_MENU_ITEMS] = {"data", "set", "information"};

    static const int SETTINGS_MENU_ITEMS = 7;
    String settingsMenuItems[SETTINGS_MENU_ITEMS] = {"WaterTime", "SleepTime", "WakeCount", "ScreenOff", "Pump", "PowerMode", "Back"};

    // 休眠设置主菜单
    static const int SLEEP_TIME_ITEMS = 3;
    String sleepTimeItems[SLEEP_TIME_ITEMS] = {"Set Hours", "Set Mins", "Back"};

    // 按键处理
    bool readButton(int pin, bool& lastState);

public:
    /**
     * @brief 构造菜单管理器。
     *
     * MenuManager 负责：
     * - 读取按键输入并进行消抖
     * - 维护当前菜单状态（页面切换、光标位置等）
     * - 根据最新传感器/网络/水泵数据刷新 OLED 显示
     *
     * 注意：构造函数只保存 oled 指针，不会初始化屏幕。
     * 初始化请调用 begin()。
     *
     * @param oledRef 指向 DisplayManager 的指针（由外部创建并保证生命周期覆盖 MenuManager）。
     */
    MenuManager(DisplayManager* oledRef);

    /**
     * @brief 菜单系统初始化。
     *
     * 典型调用时机：setup() 中，在 oled.begin() 之后调用。
     * 在此阶段会初始化菜单状态、按键状态、默认页面等。
     */
    void begin();

    /**
     * @brief 绑定可配置参数的指针。
     *
     * 菜单里修改的配置不是拷贝值，而是直接通过指针修改主程序中的变量。
     * 这使得：
     * - 菜单修改立即生效
     * - 主流程可以在后续把这些值写入 NVS 进行持久化
     *
     * @param settingsPtr  指向全局 AppSettings 的指针（浇水时长、休眠时间等）
     * @param wakeCountPtr 指向唤醒计数变量的指针（用于在菜单中调整/清零等）
     * @param powerModePtr 指向节电模式开关的指针
     */
    void bindSettings(AppSettings* settingsPtr, int* wakeCountPtr, bool* powerModePtr); // 绑定设置, 唤醒计数, 节电模式

    /**
     * @brief 刷新菜单界面显示。
     *
     * 该函数将外部传入的“最新状态数据”渲染到 OLED。
     * 建议在：
     * - 有按键事件触发菜单变化时调用
     * - 或者周期性刷新（例如每秒更新一次实时数据）
     *
     * @param moisturePercent 当前湿度百分比（用于仪表盘/数据页显示）
     * @param rssi            当前 WiFi RSSI（离线可传一个占位值）
     * @param pumpStatus      当前水泵状态（开/关）
     * @param wakeCount       当前唤醒计数（用于信息页/调试页显示）
     */
    void update(int moisturePercent, int rssi, bool pumpStatus, int wakeCount);

    /**
     * @brief 强制切换当前菜单页面。
     *
     * 用途：外部逻辑希望直接进入某个页面（例如息屏/信息页）。
     * 切换后通常需要调用 update() 才会立即反映到屏幕。
     *
     * @param newMenu 新的菜单状态枚举。
     */
    void setCurrentMenu(MenuState newMenu);

    /**
     * @brief 扫描按键并进行菜单交互处理。
     *
     * 内部会做消抖与按键状态机更新，并据此改变 currentMenu/menuIndex 等。
     *
     * 返回值语义：
     * - 返回 true：表示本次检测到“有意义的按键事件”（例如按下/确认/上下切换），
     *   外部可据此决定是否调用 update() 立即刷新屏幕。
     * - 返回 false：表示没有按键事件发生。
     *
     * @return bool 是否检测到按键事件。
     */
    bool handleButtonInput(); // 修改返回值：返回 true 表示有按键按下

    bool consumeSettingsDirty();
};

#endif