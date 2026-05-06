# ESP32 自动浇花系统

一个基于 ESP32 的植物灌溉小项目，用于解决假期宿舍植物无人照看的问题。

项目开始于大四，是我个人的练手作品。整个项目的出发点是"低成本、低门槛、能用就行"——需要的工具只有电烙铁、焊丝、电压表、一把拧可变电阻的一字螺丝刀，ESP32 开发板直接插到 PCB 上，不需要飞线或复杂焊接，其他元件焊接难度也不大。PCB 在嘉立创可以免费打样。

## 它能做什么

- **定时浇水** — 按设定的间隔唤醒、浇水，假期无人时也能让植物活下去
- **土壤湿度检测** — 模拟传感器采集数据，上报到 Blinker 物联网平台
- **节电模式** — 每 12 小时唤醒一次，隔次才联网，三节 18650 串联供电，尽量延长续航；不使用时可以把 OLED 插头拔掉进一步省电
- **OLED 屏幕 + 按键** — 0.96 寸小屏幕，三个按键，本地就能改设置
- **远程查看** — 日志自动上传到 Gitee 仓库，在网页上就能看到设备运行记录
- **远程控制** — 想改浇水参数或切换模式？在 Gitee 上编辑 `settings.json` 提交，设备下次联网时自动拉取
- **本地可视化** — 配套 Python 脚本 `data.py`，可以拉取仓库日志、绘制湿度/唤醒次数曲线、通过 GUI 界面修改远程设置并一键推送
- **OTA 升级** — 理论上支持通过 Gitee 分发固件自动更新，但目前不可用（见下方说明）

## 关于 OTA 功能

OTA 功能目前**无法正常使用**。

核心问题在于 ESP32 的 WiFi 模块在 TLS 连接下稳定性不足，导致固件下载成功率很低。尝试过多种方案（分段下载、CDN 直连、HTTP 降级等）均未取得理想效果。

个人精力有限，短期内不再继续折腾这个方向了。如果后续有时间可能会重新捡起来。代码中的 OTA 相关逻辑（`lib/gitctrl/`）保留在仓库里，默认关闭，有兴趣的话可以参考或继续开发。

## 硬件清单

| 组件 | 说明 |
|------|------|
| 主控 | ESP32 开发板（Upesy WROOM，其他 ESP32 也行） |
| 屏幕 | SSD1306 0.96" OLED，I2C 接口（可插拔，不需要时拔掉省电） |
| 传感器 | 土壤湿度传感器，模拟量输出 |
| 水泵 | 5V 微型水泵 |
| 按键 | 3 个轻触开关 |
| 供电 | 3 节 18650 锂电池串联 |

## 引脚接线

| 引脚 | 功能 |
|------|------|
| GPIO27 | 水泵控制 |
| GPIO32 | 土壤湿度传感器（模拟输入） |
| GPIO25 | 传感器供电控制 |
| GPIO21 | I2C SDA（OLED） |
| GPIO22 | I2C SCL（OLED） |
| GPIO13 | 上键 |
| GPIO14 | 选择键 |
| GPIO15 | 下键 |

## 运行状态

在标准模式下稳定运行过约**一个月**，期间定时浇水、日志上报、Blinker 数据同步均正常。更长时间的耐久性还没有测试过。

## 开发环境

使用 PlatformIO 开发和烧录。

```bash
git clone https://github.com/bowen507/ESP32_Automatic_watering_system.git
cd ESP32_Automatic_watering_system

pip install platformio
pio run                    # 编译
pio run --target upload    # 烧录
```

## 快速上手（详细步骤）

### 第一步：克隆项目

```bash
git clone https://github.com/bowen507/ESP32_Automatic_watering_system.git
cd ESP32_Automatic_watering_system
```

### 第二步：创建配置文件

把 `include/secrets.h.example` 复制一份，重命名为 `include/secrets.h`，填入你自己的配置：

```cpp
#define SECRET_GITEE_TOKEN  "你的Gitee令牌"
#define SECRET_GITEE_USER   "你的Gitee用户名"
#define SECRET_GITEE_REPO   "你的Gitee仓库名"
#define SECRET_BLINKER_AUTH "你的Blinker设备密钥"
#define SECRET_WIFI_SSID    "WiFi名称"
#define SECRET_WIFI_PASS    "WiFi密码"
```

`secrets.h` 在 `.gitignore` 里，不会被提交，不用担心泄露。

### 第三步：设置 Gitee 中转仓库

这个项目通过一个免费的 Gitee 仓库来实现远程数据查看和设置下发。你需要在 Gitee 上创建一个仓库（建议设为私有），里面放以下内容：

```
你的Gitee仓库/
├── version.json        # 版本号 + 远程设置开关
├── settings.json       # 浇水参数、休眠时长、节电模式
└── logs/               # 设备上传的运行日志（自动生成）
```

**version.json 示例：**

```json
{
    "version": "1.0.3",
    "firmware": "https://gitee.com/你的用户名/你的仓库/raw/master/firmware.bin",
    "settingonline": false,
    "settings_rev": 0
}
```

**settings.json 示例：**

```json
{
    "powerSavingMode": false,
    "globalSettings": {
        "waterTimeSeconds": 8,
        "sleepHours": 0,
        "sleepMinutes": 30
    }
}
```

创建好仓库后，将其克隆到项目的 `releases/` 文件夹：

```bash
git clone https://gitee.com/你的用户名/你的仓库.git releases
```

编译时 `copy_firmware.py` 会自动把生成的固件复制到 `releases/` 下，你需要手动把它 push 到 Gitee。

### 第四步：使用可视化工具 `data.py`

项目根目录下的 `data.py` 已配置好默认路径指向 `releases/`，克隆好 Gitee 仓库后直接运行即可：

```bash
pip install matplotlib numpy pandas gitpython PySide6
python data.py
```

功能：

- 自动 `git pull` 拉取设备最新上传的日志
- 湿度、唤醒次数随时间变化的折线图（支持按天/周/月/全部筛选）
- 右侧面板显示最新一次设备运行概览和当前生效的设置
- **控制面板**：在界面上修改浇水参数、切换节电模式，点击"确认并推送"即可提交到 Gitee，设备下次联网时自动拉取生效

也可以命令行导出图片和 CSV（适合无桌面环境）：

```bash
python data.py --export --out metrics.png --csv data.csv
```

### 远程修改设置的注意事项

通过 Gitee 修改 `settings.json` 下发设置时，**需要同时将 `version.json` 中的 `settings_rev` 加 1**。设备通过对比 `settings_rev` 判断设置是否有更新，如果不改这个值，设备不会拉取新设置。

如果使用 `data.py` 的控制面板修改，脚本会自动处理 `settings_rev` 的递增，不需要手动操作。

### 如何远程查看设备状态

两种方式：

1. **Gitee 网页** — 打开仓库的 `logs/` 目录，找到最新的 `_cur.txt` 文件，里面记录了最近一次唤醒的时间、湿度、浇水状态等
2. **Blinker APP** — 手机上实时查看土壤湿度、WiFi 信号强度

### 固件更新

如果需要更新设备固件：

1. 修改 `include/app_config.h` 中的 `CURRENT_VERSION`
2. 编译：`pio run`
3. 编译产物 `firmware.bin` 已自动复制到 `releases/`，push 到 Gitee
4. 更新 `version.json` 中的 `version` 字段和 `firmware` 下载地址
5. 设备下次维护日检测到新版本后会自动下载更新（OTA 功能当前不可用，见上文说明）

## 硬件设计

PCB 和原理图使用[嘉立创 EDA](https://lceda.cn/)设计。`hardware/` 目录下包含：

| 文件 | 说明 |
|------|------|
| `ProPrj_*.epro2` | 嘉立创 EDA 工程文件（可直接导入编辑） |
| `SCH_Schematic1_*.pdf` | 原理图 PDF |
| `Gerber_PCB2_*.zip` | Gerber 打板文件（可直接上传嘉立创下单） |
| `BOM_Board1_PCB2_*.xlsx` | 物料清单 |

> 如果想直接在嘉立创 EDA 中编辑，可以将工程导出为 `.json` 文件放入此目录。

## 项目结构

```
├── src/main.cpp                  # 主程序
├── include/
│   ├── pins_config.h             # 引脚定义
│   ├── app_config.h              # 版本号
│   └── secrets.h.example         # 配置模板
├── lib/
│   ├── gitctrl/                  # OTA 更新（当前不可用）
│   ├── logger/                   # 日志缓存与上传
│   ├── menu/                     # OLED 菜单
│   ├── network/                  # WiFi / NTP / 深度休眠
│   ├── NVS/                      # NVS 持久化存储
│   └── OLED/                     # SSD1306 驱动
├── releases/                     # Gitee 中转仓库（克隆到这里）
├── hardware/                     # PCB 和原理图
├── data.py                       # 日志可视化与远程控制工具
├── copy_firmware.py              # 编译后自动复制固件到 releases/
├── platformio.ini
└── partitions.csv
```

## 关于开源协议

本项目使用 [MIT](LICENSE) 协议。
---

*一个大四学生的练手项目，希望能帮到有类似需求的人。*
