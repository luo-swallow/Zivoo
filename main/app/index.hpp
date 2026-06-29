#pragma once
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "model/image_process.hpp"
#include "model/coco_classes.hpp"
#include "who_cam_define.hpp"
#include "who_frame_cap.hpp"
#include "dl_detect_base.hpp"
#include "wifi_manager.hpp"
#include "llm_client.hpp"
#include "speech_recognizer.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <functional>
#include <map>

namespace who {
namespace ui {

// LLM 工作线程内部接口
struct LLMTaskMsg {
    void (*func)(void*);
    void* params;
};
void llm_worker_init();
void llm_worker_send(LLMTaskMsg* msg);

// UI 容器尺寸配置（LVGL容器大小）
constexpr uint16_t CAMERA_CONTAINER_W = 525;
constexpr uint16_t CAMERA_CONTAINER_H = 525;

// PPA缩放尺寸（实际显示帧大小）
constexpr uint16_t PPA_DISP_W = 500;
constexpr uint16_t PPA_DISP_H = 500;

// 图片显示尺寸
constexpr uint16_t IMAGE_DISP_W = 500;
constexpr uint16_t IMAGE_DISP_H = 500;

// 检测调色板颜色
constexpr std::array<uint8_t, 3> DETECT_PALETTE_COLOR = {255, 0, 0};  // 红色检测框

class MyAppUI {
public:
    MyAppUI();
    ~MyAppUI();

    // 初始化 UI
    bool init();

    // 获取摄像头容器（用于 WhoDetectAppLCD）
    lv_obj_t *get_camera_container();

    // 获取结果容器（用于显示检测结果）
    lv_obj_t *get_result_container();

    // 更新检测结果显示
    void update_detect_result(int count, const char *info);

    // 设置帧捕获节点（用于Capture功能）
    void set_frame_cap_node(who::frame_cap::WhoFrameCapNode *node);

    // 设置检测模型（用于Start功能）
    void set_detect_model(dl::detect::Detect *model);

    // 初始化图片管理器
    void init_image_manager();

    // 获取当前模式（用于外部判断）
    bool is_identify_mode() { return m_identify_mode; }
    bool is_chat_mode() { return m_chat_mode; }

    // Assist 智能识别：供外部检测回调调用
    void feed_detect_result(const std::list<dl::detect::result_t> &det_res);

    // 更新 WiFi 按钮状态显示（供外部回调调用）
    void update_wifi_button_state(who::wifi::WifiState state);

    // 更新性能标签
    void update_infer_time(int64_t ms);
    void update_sr_time(int64_t ms);

    // 摄像头控制回调类型
    typedef std::function<void()> camera_control_cb_t;

    // 设置摄像头暂停/恢复回调
    void set_camera_pause_cb(camera_control_cb_t cb) { m_camera_pause_cb = cb; }
    void set_camera_resume_cb(camera_control_cb_t cb) { m_camera_resume_cb = cb; }

    // 执行语音命令（供语音识别模块调用）
    void execute_voice_command(speech::VoiceCommand cmd);

private:
    // 导航栏组件
    lv_obj_t *m_nav_bar;
    lv_obj_t *m_title_label;
    lv_obj_t *m_perf_infer_label;   // 推理耗时标签
    lv_obj_t *m_perf_sr_label;      // 语音识别耗时标签
    lv_obj_t *m_settings_btn;       // 设置按钮（最右侧）
    lv_obj_t *m_settings_container; // 设置弹出容器
    bool m_settings_open;           // 设置容器是否打开
    lv_obj_t *m_wifi_btn;           // WiFi 按钮（在设置容器内）
    lv_obj_t *m_voice_btn;          // Voice 开关按钮（在设置容器内）
    lv_obj_t *m_llm_dropdown;       // LLM 下拉菜单（在设置容器内）
    lv_obj_t *m_settings_overlay;   // 全屏透明覆盖层（点击外部关闭用）
    bool m_voice_enabled;           // 语音输出开关
    lv_obj_t *m_assist_btn;         // Assist 开关按钮（在设置容器内）
    bool m_assist_enabled;          // 智能识别开关

    // 主内容区域
    lv_obj_t *m_content_area;       // 主内容区域（横向布局）
    lv_obj_t *m_camera_container;   // 左侧摄像头容器 (525x525)
    lv_obj_t *m_right_container;    // 右侧容器（垂直布局，包含结果和按钮）
    lv_obj_t *m_result_container;   // 结果容器
    lv_obj_t *m_result_title_label; // "识别结果" 标题标签
    lv_obj_t *m_table_header;       // 表头行（Seq | Name | Conf）
    std::vector<lv_obj_t *> m_result_textareas; // 每个物种的 textarea

    // 图片显示相关（用于识别浏览模式）
    lv_obj_t *m_image_canvas;       // 静态图片显示canvas (500x500)
    uint8_t *m_image_buffer;        // 图片显示缓冲区
    bool m_identify_mode;           // 是否处于识别浏览模式
    image::ImageManager *m_image_manager;  // 图片管理器

    // 帧捕获节点（用于Capture功能）
    who::frame_cap::WhoFrameCapNode *m_frame_cap_node;

    // 检测模型（用于单图检测）
    dl::detect::Detect *m_detect_model;

    // 操作按钮
    lv_obj_t *m_capture_btn;        // Capture 按钮
    lv_obj_t *m_identify_btn;       // Identify/Back 按钮
    lv_obj_t *m_image_count_label;  // 图片计数标签 (第几张/总数量)
    lv_obj_t *m_delete_btn;         // Delete 删除按钮
    lv_obj_t *m_browse_container;   // 浏览按钮容器
    lv_obj_t *m_pre_btn;            // Pre 上一张按钮
    lv_obj_t *m_start_btn;          // Start 开始按钮
    lv_obj_t *m_next_btn;           // Next 下一张按钮
    lv_obj_t *m_ai_chat_btn;        // AiChat 按钮（初始隐藏，识别成功后显示）

    // AI Chat 相关
    lv_obj_t *m_chat_container;     // 聊天输入容器（文本框+Send按钮）
    lv_obj_t *m_chat_input;         // 文本输入框
    lv_obj_t *m_send_btn;           // Send 发送按钮
    lv_obj_t *m_keyboard;           // 拼音键盘
    lv_obj_t *m_ime_pinyin;         // 拼音输入法
    bool m_chat_mode;               // 是否处于聊天模式
    bool m_llm_task_cancelled;      // LLM 任务取消标志（切换模式时设置）
    std::string m_detection_context; // 检测结果上下文（用于 AI Chat）

    // 摄像头控制回调
    camera_control_cb_t m_camera_pause_cb;    // 摄像头暂停回调
    camera_control_cb_t m_camera_resume_cb;   // 摄像头恢复回调

    // 实时检测历史记录（摄像头模式下，每次检测追加一条）
    std::vector<lv_obj_t *> m_detect_history;  // 历史记录标签列表
    static constexpr int DETECT_HISTORY_MAX = 18;
    uint32_t m_detect_seq;

    // Assist 智能识别相关
    struct AssistDetectRecord {
        int category;
        int64_t timestamp_ms;  // 检测时刻（毫秒）
    };
    std::vector<AssistDetectRecord> m_assist_records;  // 近期检测记录
    bool m_assist_capture_pending;  // 是否正在等待 auto-capture 完成
    static constexpr int ASSIST_WINDOW_MS = 5000;       // 5秒检测窗口
    static constexpr int ASSIST_MIN_COUNT = 3;         // 5秒内最少检测次数阈值
    static void onAssistClicked(lv_event_t *e);
    void assist_on_detect_result(const std::list<dl::detect::result_t> &det_res);
    void assist_trigger_capture();

    // 按钮点击事件回调
    static void onCaptureClicked(lv_event_t *e);
    static void onIdentifyClicked(lv_event_t *e);
    static void onDeleteClicked(lv_event_t *e);
    static void onPreClicked(lv_event_t *e);
    static void onStartClicked(lv_event_t *e);
    static void onNextClicked(lv_event_t *e);
    static void onWifiClicked(lv_event_t *e);
    static void onVoiceClicked(lv_event_t *e);
    static void onSettingsClicked(lv_event_t *e);
    static void onLLMDropdownChanged(lv_event_t *e);
    static void onAiChatClicked(lv_event_t *e);
    static void onSendClicked(lv_event_t *e);
    static void onOverlayClicked(lv_event_t *e);

    // 模式切换方法
    void switch_to_identify_mode();
    void switch_to_camera_mode();

    // 更新图片显示
    void update_image_display();

    // 更新图片计数标签
    void update_image_count_label();

    // 执行单图检测
    void perform_single_image_detect();

    // 创建图片canvas
    void create_image_canvas();

    // 清除旧的识别结果 textarea
    void clear_result_textareas();
};

} // namespace ui
} // namespace who
