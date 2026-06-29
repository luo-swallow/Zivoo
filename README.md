# Zivoo (知物) - 边缘端AI 物体识别与多模态交互系统

## 📖 项目简介

&emsp;&emsp;在日常生活中，我们常会遇到不认识的植物、动物或物品，想要了解它们的名称、特性、文化背景、生活习性等知识。无论是户外徒步时偶遇的奇特植物，陌生的动物，还是博物馆展览中的历史文物，我们都渴望深入了解它们背后的故事和知识。Zivoo（知物）正是为解决这些需求而设计的便携式知识助手。

&emsp;&emsp;这款基于 **ESP32-P4** 的智能识别系统集成了**本地视觉处理**、**云端大语言模型交互**、**本地SR语音识别**，通过多模态交互、本地和云端能力，让用户更智能体验。系统结合了嵌入式 AI 的高效性和现代交互的便捷性，为户外探索、旅行科普、日常学习等场景提供贴心帮助，让知识获取变得简单、直观、高效。


## ✨ 项目功能
- **实时物体识别**: 通过摄像头捕捉图像，使用本地 YOLO 模型实时识别物体类别,打印边界框，显示物体类别和置信度
- **科普信息展示**: 在科普界面模式下，点击启动识别按钮，系统将显示物体基本信息，联网后，基于云端LLM模型，可获取详细科普介绍
- **语音控制**: 支持中文语音命令，实现拍照、识别、发送等命令操作
- **智能问答**: 集成云端 LLM 模型，可进行自然语言对话和知识问答


## 🚀 快速开始

### 硬件需求
- **ESP32-P4-Function-EV-Board** — 主控板
- **OV5647 摄像头模组（15P-FPC）** — MIPI CSI 接口
- **7 英寸 MIPI DSI 触摸式显示屏** — 分辨率 1024×600
- **4Ω 3W 扬声器** — 用于 TTS 语音播报
- **麦克风模块** — 用于语音识别（ESP-SR）
- **SD 卡** — 存储图片、YOLO 模型及 SR 模型文件
- **拍照按钮**（可选）— GPIO 检测高电平触发
- **3D 打印外壳**（可选）— 将主控板、屏幕、摄像头、扬声器集成为一体化便携设备


### 软件环境
1. **ESP-IDF** 环境配置（本项目基于v5.4.3开发）

### 代码配置
在编译烧录前，请根据需要修改以下配置信息：

> **⚠️ 模型文件提醒**: YOLO 视觉检测模型 (`coco_detect_yolo11n_320_s8_v3.espdl`) 和 ESP-SR 语音识别模型均**不随固件烧录到 Flash**，需手动放置到 **SD 卡**对应目录。详见下方各模型配置说明。

#### WiFi 配置
修改 `main/app/wifi_manager.cpp` 第 12-13 行：
```cpp
#define WIFI_SSID "your_wifi_ssid"      // WiFi 名称
#define WIFI_PASSWORD "your_wifi_password"  // WiFi 密码
```

#### 本地视觉模型配置
本项目使用 YOLO11n 模型进行本地物体检测。

**模型文件**: `coco_detect_yolo11n_320_s8_v3.espdl`（项目路径：`main/model/`）

**⚠️ 重要**: 模型存放在 SD 卡而非 Flash，用户需将 `main/model/coco_detect_yolo11n_320_s8_v3.espdl` 复制到 SD 卡 `/sdcard/models/p4/` 目录下。

修改 `main/model/detect.hpp` 第 27 行可更换模型：
```cpp
dl::detect::Detect *get_detect_model(const char *model_name = "your_model.espdl");
```

修改 `main/sys.hpp` 第 23-24 行可调整模型输入尺寸：
```cpp
#define MODEL_INPUT_W 320   // 模型输入宽度
#define MODEL_INPUT_H 320   // 模型输入高度
```

#### 本地语音模型配置
本项目使用 ESP-SR 框架进行本地语音识别，模型存放在 **SD 卡**。


**唤醒词**: "嗨乐鑫" (HiLexin)，使用 WakeNet 模型唤醒，MultiNet 模型进行命令词识别。

**SD 卡路径**: 模型需放置在 SD 卡 `/sdcard/models/sr/` 目录下（代码中通过 `esp_srmodel_init("/sdcard/models/sr")` 加载）。

**模型来源**: SR 模型文件位于项目 `main/sr/` 目录下，使用者需将该目录下的全部内容复制到 SD 卡的 `/sdcard/models/sr/` 目录中：

```
main/sr/
├── wn9_hilexin/        # 唤醒词模型（"嗨乐鑫"）
├── mn7_cn/             # 命令词识别模型（中文 MultiNet7）
├── vadnet1_medium/     # 语音活动检测模型
└── fst/                # FST 解码文件
```

#### 云端 LLM 配置
修改 `main/app/llm_client.cpp` 第 41-44 行（支持自定义 OpenAI 协议 API）：
```cpp
static constexpr const char* LLM_API_URL = "https://api.xxxxx.com/v1/chat/completions";  // API 地址
static constexpr const char* LLM_API_KEY = "sk-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"; // API Key
static constexpr const char* LLM_MODEL = "xxxxx";             // 语言/聊天模型（AiChat、科普）
static constexpr const char* LLM_VISION_MODEL = "xxxxx";     // 视觉模型（可与 LLM_MODEL 不同）
```


## 🔧 内容介绍

### 👁️ Vision（视觉处理模块）
基于 **ESP-DL、ESP-WHO** 框架的本地 AI 视觉处理系统，使用 YOLO11n 模型进行实时物体检测，可识别 COCO 80 种物体类别。

**使用方法**：
- **实时预览**: 显示摄像头实时画面，自动标记识别到的物体
- **拍照**: 点击拍照按钮、语音说"帮我拍照"、或通过 GPIO_NUM_23 高电平检测触发外接按钮保存当前帧
- **WiFi 连接**: 点击右上角 WiFi 图标连接网络

### 💬 云端 LLM（大语言模型交互）
集成云端 LLM 模块，提供 Language 和 Vision 两种模式，用户可通过导航栏下拉菜单切换。

**Language 模式（语言模式）**：
- 纯文本交互，基于 YOLO 检测到的物种名称生成科普介绍
- 响应速度快，输出格式：物种名称 + 科普介绍（150-200字）

请求数据格式示例：
```json
{
  “model”: “xxxxxxxxx”,
  “stream”: false,
  “response_format”: {
    “type”: “json_object”
  },
  “messages”: [
    {
      “role”: “system”,
      “content”: “xxxxxxxxxx”
    },
    {
      “role”: “user”,
      “content”: “xxxxxxxxxx”
    }
  ]
}
```

**Vision 模式（视觉模式）(需要云端LLM支持视觉功能)**：
- 多模态交互，同时发送图片（base64编码）和 YOLO 检测结果
- 云端 AI 进行精细化识别，可将泛称精确到具体品种

请求数据格式示例：
```json
{
  “model”: “xxxxxxxxxx”,
  “stream”: false,
  “max_completion_tokens”: 2048,
  “response_format”: {
    “type”: “json_object”
  },
  “messages”: [
    {
      “role”: “user”,
      “content”: [
        {
          “type”: “image_url”,
          “image_url”: {
            “url”: “data:image/jpeg;base64,xxxxxxxxx”
          }
        },
        {
          “type”: “text”,
          “text”: “xxxxxxxxxxx”
        }
      ]
    }
  ]
}
```

**使用方法**：
- **识别浏览**: 使用上一张/下一张按钮或左右滑动浏览已保存图片
- **单图识别**: 选择图片后点击启动识别按钮，显示物体类别、置信度和科普信息
- **AI 聊天**: 识别后点击Ai聊天按钮，使用拼音键盘输入问题，点击发送发送

### 🎤 SR（语音识别）
基于 **ESP-SR** 框架的本地语音识别，唤醒词”嗨乐鑫”，支持中文语音命令。

**使用方法**：
- **唤醒设备**: 说出唤醒词”嗨乐鑫”激活语音控制
- **执行命令**: 说出预定义命令，如：
  - “帮我拍照”: 拍摄当前画面并且识别一次
  - “帮我识别”: 对当前画面进行识别
  - “帮我删除照片”: 删除当前图片
  - “发送”: 在聊天模式中发送消息
  - “上一张”/”下一张”: 浏览图片
  - “加载网络”: 连接/断开 WiFi 连接
  - “关闭声音”/”打开声音”: 开关 TTS 语音播报
  - “打开助手模式”/”关闭助手模式”: 开关助手模式

**TTS 语音模型**: 使用 ESP-SR 内置的中文 TTS 引擎，语音数据文件位于 `managed_components/espressif__esp-sr/esp-tts/esp_tts_chinese/esp_tts_voice_data_xiaoxin.dat`（小新音色）。该文件在编译时通过 CMake 自动烧录到 Flash 的 `voice_data` 分区（FAT，3890K），无需手动操作。

**扬声器语音播报**（关闭声音开关关闭时自动播放）：
- **唤醒响应**: “我在”
- **识别相关**: “正在识别”、”识别完成”、”识别失败”
- **网络相关**: “正在连接网络”、”网络连接成功”、”网络已断开”、”网络连接失败”
- **助手模式**: “助手模式已打开”、”助手模式已关闭”
- **语音控制**: “语音关闭成功”、”语音打开成功”

### 🖥️ LVGL 图形界面
基于 **LVGL** 的现代图形用户界面，提供直观的触摸交互体验。

```
┌──────────────────────────────────────────────────────────────────┐
│ Zivoo                   [拍照] [科普界面] [设置]                   │
└──────────────────────────────────────────────────────────────────┘
┌──────────────────────────────────────────────────────────────────┐
│                        主内容区域 (550px)                         │
│ ┌────────────────────┐  ┌─────────────────────────────────────┐ │
│ │                    │  │ 识别结果 (400×420)                  │ │
│ │                    │  │ ───────────────────────────────    │ │
│ │   摄像头/图片      │  │ 序号  名称    置信度                │ │
│ │   容器: 525×525    │  │  1    xxx     90%                  │ │
│ │   画面: 500×500    │  │  2    xxx     85%                  │ │
│ │                    │  │ 介绍内容...                        │ │
│ │   橙色边框         │  └─────────────────────────────────────┘ │
│ │                    │  ┌──────┐ ┌──────────┐                   │
│ │                    │  │ 0/0  │ │ 去除照片  │                   │
│ │                    │  └──────┘ └──────────┘                   │
│ │                    │  ┌──────┐ ┌──────┐ ┌──────┐              │
│ │                    │  │上一张│ │启动识别│ │下一张│              │
│ └────────────────────┘  └──────┘ └──────┘ └──────┘              │
└──────────────────────────────────────────────────────────────────┘
```


## 📁 项目结构

```
zivoo/
├── main/                         # 主应用程序代码
│   ├── app/                     # 应用程序模块
│   │   ├── index.cpp/hpp        # 主 UI 实现
│   │   ├── llm_client.cpp/hpp   # 云端 LLM 客户端
│   │   ├── speech_recognizer.cpp/hpp  # 语音识别
│   │   ├── wifi_manager.cpp/hpp # WiFi 管理
│   │   └── splash.cpp/hpp       # 开机动画
│   ├── model/                   # AI 模型相关
│   │   ├── detect.cpp/hpp       # 检测模型
│   │   ├── image_process.cpp/hpp # 图像处理
│   │   ├── coco_classes.cpp/hpp # COCO 类别
│   │   └── local_knowledge.json # 本地科普知识库
│   ├── sr/                      # 语音识别模型文件
│   │   ├── wn9_hilexin/         # 唤醒词模型
│   │   ├── mn7_cn/              # 命令词识别模型
│   │   ├── vadnet1_medium/      # 语音活动检测模型
│   │   └── fst/                 # FST 解码文件
│   ├── sys.cpp/hpp              # 系统工具函数（TTS 等）
│   └── main.cpp                 # 程序入口
├── who_components/              # 自定义组件
│   ├── who_app/                 # 应用程序组件
│   ├── who_detect/              # 检测组件
│   ├── who_frame_cap/           # 帧捕获组件
│   ├── who_frame_lcd_disp/      # 帧显示组件
│   ├── who_peripherals/         # 外设组件
│   ├── who_qrcode/              # 二维码组件
│   ├── who_recognition/         # 识别组件
│   └── who_task/                # 任务管理组件
└── managed_components/          # ESP-IDF 组件管理
```

**SD 卡文件结构**（需手动将模型文件复制到 SD 卡）：

```
sdcard/
└── models/
    ├── p4/                                    # YOLO 视觉检测模型
    │   └── coco_detect_yolo11n_320_s8_v3.espdl   # ← 来自 main/model/
    └── sr/                                    # ESP-SR 语音识别模型
        ├── wn9_hilexin/                       # ← 来自 main/sr/wn9_hilexin/
        │   └── ...                            # 唤醒词模型（"嗨乐鑫"）
        ├── mn7_cn/                            # ← 来自 main/sr/mn7_cn/
        │   └── ...                            # 命令词识别模型（中文 MultiNet7）
        ├── vadnet1_medium/                    # ← 来自 main/sr/vadnet1_medium/
        │   └── ...                            # 语音活动检测模型
        └── fst/                               # ← 来自 main/sr/fst/
            └── ...                            # FST 解码文件
```

---

**Zivoo - 让知识触手可及** 🌿🔍

*如果您觉得这个项目有帮助，请给我们一个 Star ⭐ 支持我们的工作！*