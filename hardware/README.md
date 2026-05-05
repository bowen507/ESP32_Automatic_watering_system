# 硬件设计文件

请将以下文件放入此目录：

## 嘉立创 EDA 导出方法

1. 在[嘉立创 EDA](https://lceda.cn/)中打开工程
2. 文件 → 导出 → **嘉立创 EDA 工程文件 (.json)**
3. 文件 → 导出 → Gerber（打板用）
4. 文件 → 导出 → BOM（物料清单）
5. 文件 → 导出 → Pick and Place（SMT 贴片用）
6. 原理图界面 → 导出 → PDF
7. PCB 界面 → 视图 → 3D 预览 → 截图

## 建议的文件列表

```
hardware/
├── ESP32_WATER.json          # 嘉立创 EDA 工程文件
├── schematic.pdf             # 原理图 PDF
├── schematic.png             # 原理图截图
├── pcb_front.png             # PCB 正面 3D 渲染
├── pcb_back.png              # PCB 背面 3D 渲染
├── gerber/                   # Gerber 打板文件
├── BOM.csv                   # 物料清单
└── README.md                 # 本文件
```
