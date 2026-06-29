#pragma once

#include "who_frame_cap.hpp"
#include "who_cam.hpp"
#include "driver/gpio.h"
#include <functional>

namespace who {
namespace sys {

// TTS 语音合成
bool tts_init();
void tts_speak(const char *text);
void tts_stop();

// 本地知识库（从 SD 卡加载 JSON）
bool knowledge_init();
const char* knowledge_query(const char* key);
void knowledge_deinit();

// 模型配置参数
#define MODEL_TIME 5        // 模型推理超时时间(秒)
#define MODEL_INPUT_W 320   // 模型输入宽度
#define MODEL_INPUT_H 320   // 模型输入高度

// LCD显示配置参数（PPA缩放尺寸）
#define LCD_DISP_W 500      // PPA硬件缩放宽度
#define LCD_DISP_H 500      // PPA硬件缩放高度

// 按钮配置参数
#define BUTTON_GPIO_DEFAULT GPIO_NUM_23  // 默认按钮GPIO引脚

// 按钮回调类型
typedef std::function<void()> button_callback_t;

/**
 * @brief 创建LCD MIPI CSI + PPA缩放的帧捕获pipeline
 * @param lcd_disp_frame_cap_node 输出参数，返回LCD显示用的帧捕获节点
 * @return WhoFrameCap* 帧捕获pipeline
 *
 * 流程: CSI摄像头 -> PPA硬件缩放(500x500) -> LCD显示
 *       CSI摄像头原始帧 -> PPA硬件缩放(320x320) -> 模型推理
 */
who::frame_cap::WhoFrameCap *get_lcd_mipi_csi_ppa_frame_cap_pipeline(
    who::frame_cap::WhoFrameCapNode **lcd_disp_frame_cap_node);

/**
 * @brief 初始化按钮
 * @param gpio_num 按钮GPIO引脚号，默认为GPIO_NUM_23
 * @return true 成功, false 失败
 */
bool button_init(gpio_num_t gpio_num = BUTTON_GPIO_DEFAULT);

/**
 * @brief 设置按钮按下回调函数
 * @param callback 回调函数，在检测到高电平（上升沿）时调用
 */
void button_set_callback(button_callback_t callback);

/**
 * @brief 启动按钮检测任务
 */
void button_start();

/**
 * @brief 停止按钮检测任务
 */
void button_stop();

/**
 * @brief 获取按钮当前电平
 * @return 0: 低电平, 1: 高电平
 */
int button_get_level();

} // namespace sys
} // namespace who