#include "menu_manager.h"
#include "app_config.h"
#include <Arduino.h>

MenuManager::MenuManager(DisplayManager* oledRef) : oled(oledRef), currentMenu(MENU_DASHBOARD), menuIndex(0) {
    lastButtonCheckTime = 0;
    isDisplayOn = true;
    settings = nullptr;
    wakeupCountMenuPtr = nullptr;
    powerModePtr = nullptr;
    settingsDirty = false;
}

bool MenuManager::consumeSettingsDirty() {
    bool wasDirty = settingsDirty;
    settingsDirty = false;
    return wasDirty;
}

void MenuManager::begin() {
    pinMode(BTN_UP_PIN, INPUT_PULLUP);
    pinMode(BTN_SELECT_PIN, INPUT_PULLUP);
    pinMode(BTN_DOWN_PIN, INPUT_PULLUP);

    lastUpState = digitalRead(BTN_UP_PIN);
    lastSelectState = digitalRead(BTN_SELECT_PIN);
    lastDownState = digitalRead(BTN_DOWN_PIN);
    lastDebounceTime = millis();
}

void MenuManager::bindSettings(AppSettings* settingsPtr, int* wakeCountPtr, bool* powerMode) {
    settings = settingsPtr;
    wakeupCountMenuPtr = wakeCountPtr;
    powerModePtr = powerMode;
}

bool MenuManager::readButton(int pin, bool& lastState) {
    bool currentState = digitalRead(pin); // 读取当前状态
    bool isPressed = false;

    // 边缘检测：只有当状态从 HIGH (松开) 变为 LOW (按下) 的一瞬间，才认为是触发
    // 假设使用了 INPUT_PULLUP，按下为 LOW
    if (lastState == HIGH && currentState == LOW) {
        // 防抖：检查距离上次触发是否超过 200ms
        if (millis() - lastDebounceTime > 200) {
            isPressed = true;
            lastDebounceTime = millis(); // 更新防抖计时
        }
    }

    lastState = currentState; // 更新状态供下次比较
    return isPressed;
}

void MenuManager::update(int moisturePercent, int rssi, bool pumpStatus, int wakeCount) {
    // 如果屏幕关闭，不进行任何显示更新
    if (!isDisplayOn) return;

    // 根据当前菜单状态显示不同内容
    switch (currentMenu) {
        case MENU_DASHBOARD:
            // 显示实时数据
            oled->update(moisturePercent, rssi, pumpStatus, wakeCount);
            break;

        case MENU_MAIN:
            oled->showMenu("Mainmenu", mainMenuItems, MAIN_MENU_ITEMS, menuIndex);
            break;

        case MENU_SETTINGS:
            oled->showMenu("Settings", settingsMenuItems, SETTINGS_MENU_ITEMS, menuIndex);
            break;

        case MENU_WATER_TIME:
            if (settings) oled->showMessage("Water Time:\n < " + String(settings->waterTimeSeconds) + " s >");
            break;

        case MENU_SLEEP_TIME:
            oled->showMenu("Sleep Time", sleepTimeItems, SLEEP_TIME_ITEMS, menuIndex);
            break;

        case MENU_SLEEP_HOURS:
            if (settings) oled->showMessage("Sleep Hours:\n < " + String(settings->sleepHours) + " h >");
            break;

        case MENU_SLEEP_MINUTES:
            if (settings) oled->showMessage("Sleep Mins:\n < " + String(settings->sleepMinutes) + " m >");
            break;

        case MENU_WAKEUP_COUNT:
            if (wakeupCountMenuPtr) oled->showMessage("Wake Count:\n < " + String(*wakeupCountMenuPtr) + " >");
            break;

        case MENU_MOISTURE_SET:
            oled->showMessage("Moisture Set\nTarget: 50%");
            break;

        case MENU_PUMP_CONTROL:
            oled->showMessage(pumpStatus ? "Pump: ON" : "Pump: OFF");
            break;

        case MENU_POWER_MODE:
            if (powerModePtr) oled->showMessage(*powerModePtr ? "Power Save:\n < ON >" : "Power Save:\n < OFF >");
            break;

        case MENU_SCREEN_OFF:
            // 理论上不会进入这里，因为进入此状态时屏幕已关
            break;

        case MENU_INFO:
            oled->showMessage("System Info\nVer: " + String(CURRENT_VERSION) + "\nWake: " + String(wakeCount));
            break;
    }
}

bool MenuManager::handleButtonInput() {
    bool upPressed = readButton(BTN_UP_PIN, lastUpState);
    bool selectPressed = readButton(BTN_SELECT_PIN, lastSelectState);
    bool downPressed = readButton(BTN_DOWN_PIN, lastDownState);

    // 1. 处理息屏唤醒逻辑
    if (!isDisplayOn) {
        if (selectPressed) {
            isDisplayOn = true;
            oled->setPower(true);
            setCurrentMenu(MENU_DASHBOARD); // 唤醒后回到仪表盘
            return true; // 触发刷新
        }
        return false; // 息屏状态下忽略其他按键
    }

    bool anyButtonPressed = false;

    // 单独处理仪表盘模式的按键
    if (currentMenu == MENU_DASHBOARD) {
        if (selectPressed) {
            setCurrentMenu(MENU_MAIN); // 按确定进入主菜单
            return true;
        }
        // 在仪表盘模式下，上下键不执行操作
        return false;
    }

    // 判断当前是否为"数值编辑"模式
    bool isEditMode = (currentMenu == MENU_WATER_TIME ||
                       currentMenu == MENU_SLEEP_HOURS ||
                       currentMenu == MENU_SLEEP_MINUTES ||
                       currentMenu == MENU_WAKEUP_COUNT ||
                       currentMenu == MENU_POWER_MODE); // 增加 Power Mode

    if (isEditMode) {
        // --- 数值编辑模式逻辑 ---
        if (upPressed) {
            anyButtonPressed = true;
            if (settings) {
                switch (currentMenu) {
                    case MENU_WATER_TIME: settings->waterTimeSeconds++; settingsDirty = true; break;
                    case MENU_SLEEP_HOURS: settings->sleepHours++; settingsDirty = true; break;
                    case MENU_SLEEP_MINUTES: settings->sleepMinutes++; settingsDirty = true; break;
                    case MENU_WAKEUP_COUNT: if(wakeupCountMenuPtr) (*wakeupCountMenuPtr)++; break;
                    case MENU_POWER_MODE: if(powerModePtr) { *powerModePtr = !(*powerModePtr); settingsDirty = true; } break;
                }
            }
        }
        if (downPressed) {
            anyButtonPressed = true;
            if (settings) {
                switch (currentMenu) {
                    case MENU_WATER_TIME:
                        if (settings->waterTimeSeconds > 1) { settings->waterTimeSeconds--; settingsDirty = true; }
                        break;
                    case MENU_SLEEP_HOURS:
                        if (settings->sleepHours > 0) { settings->sleepHours--; settingsDirty = true; }
                        break;
                    case MENU_SLEEP_MINUTES:
                        if (settings->sleepMinutes > 0) { settings->sleepMinutes--; settingsDirty = true; }
                        break;
                    case MENU_WAKEUP_COUNT:
                        if(wakeupCountMenuPtr && *wakeupCountMenuPtr > 0) (*wakeupCountMenuPtr)--;
                        break;
                    case MENU_POWER_MODE: if(powerModePtr) { *powerModePtr = !(*powerModePtr); settingsDirty = true; } break;
                }
            }
        }
        if (selectPressed) {
            anyButtonPressed = true;
            // 确认并返回上一级
            switch (currentMenu) {
                case MENU_WATER_TIME: setCurrentMenu(MENU_SETTINGS); break;
                case MENU_SLEEP_HOURS: setCurrentMenu(MENU_SLEEP_TIME); break;
                case MENU_SLEEP_MINUTES: setCurrentMenu(MENU_SLEEP_TIME); break;
                case MENU_WAKEUP_COUNT: setCurrentMenu(MENU_SETTINGS); break;
                case MENU_POWER_MODE: setCurrentMenu(MENU_SETTINGS); break;
            }
        }
    } else {
        // --- 列表导航模式逻辑 (原有逻辑) ---
        if (downPressed) {
            anyButtonPressed = true;
            menuIndex--;
            if (menuIndex < 0) {
                // 根据当前菜单循环选择
                switch (currentMenu) {
                    case MENU_MAIN: menuIndex = MAIN_MENU_ITEMS - 1; break;
                    case MENU_SETTINGS: menuIndex = SETTINGS_MENU_ITEMS - 1; break;
                    case MENU_SLEEP_TIME: menuIndex = SLEEP_TIME_ITEMS - 1; break;
                    default: menuIndex = 0; break;
                }
            }
        }

        if (upPressed) {
            anyButtonPressed = true;
            menuIndex++;
            switch (currentMenu) {
                case MENU_MAIN:
                    if (menuIndex >= MAIN_MENU_ITEMS) menuIndex = 0;
                    break;
                case MENU_SETTINGS:
                    if (menuIndex >= SETTINGS_MENU_ITEMS) menuIndex = 0;
                    break;
                case MENU_SLEEP_TIME:
                    if (menuIndex >= SLEEP_TIME_ITEMS) menuIndex = 0;
                    break;
                    default: menuIndex = 0; break;
            }
        }

        if (selectPressed) {
            anyButtonPressed = true;
            switch (currentMenu) {
                case MENU_MAIN:
                    switch (menuIndex) {
                        case 0: setCurrentMenu(MENU_DASHBOARD); break;; // 已经在显示实时数据
                        case 1: setCurrentMenu(MENU_SETTINGS); menuIndex = 0; break;
                        case 2: setCurrentMenu(MENU_INFO); break;
                    }
                    break;

                case MENU_SETTINGS:
                    switch (menuIndex) {
                        case 0: setCurrentMenu(MENU_WATER_TIME); break; // WaterTime
                        case 1: setCurrentMenu(MENU_SLEEP_TIME); break; // SleepTime
                        case 2: setCurrentMenu(MENU_WAKEUP_COUNT); break; // WakeCount
                        case 3: // Screen Off
                            isDisplayOn = false;
                            oled->setPower(false);
                            setCurrentMenu(MENU_SCREEN_OFF);
                            break;
                        case 4: setCurrentMenu(MENU_PUMP_CONTROL); break; // Pump
                        case 5: setCurrentMenu(MENU_POWER_MODE); break; // 新增 Power Mode
                        case 6: setCurrentMenu(MENU_MAIN); menuIndex = 1; break; // Back
                    }
                    break;

                case MENU_SLEEP_TIME:
                    switch (menuIndex) {
                        case 0: setCurrentMenu(MENU_SLEEP_HOURS); break;
                        case 1: setCurrentMenu(MENU_SLEEP_MINUTES); break;
                        case 2: setCurrentMenu(MENU_SETTINGS); menuIndex = 1; break; // Back
                    }
                    break;

                // 在子菜单中按下选择键返回
                case MENU_MOISTURE_SET:
                case MENU_PUMP_CONTROL:
                case MENU_INFO:
                    setCurrentMenu(MENU_MAIN);
                    break;

                default:
                    setCurrentMenu(MENU_MAIN);
                    menuIndex = 1; // 返回到设置菜单项
                    break;
            }
        }
    }
    return anyButtonPressed;
}


void MenuManager::setCurrentMenu(MenuState newMenu) {
    currentMenu = newMenu;
    menuIndex = 0; // 重置选择索引
}