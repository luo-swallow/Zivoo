#pragma once

#include "lvgl.h"
#include <functional>
#include <string>

namespace who {
namespace speech {

// 语音命令ID定义
enum class VoiceCommand {
    CMD_NONE = 0,
    CMD_PAI_ZHAO,       // 帮我拍照 -> Capture
    CMD_SHI_BIE,        // 帮我识别 -> Identify + Start
    CMD_SHAN_CHU,       // 帮我删除照片 -> Delete
    CMD_XIA_YI_ZHANG,   // 下一张 -> Next
    CMD_SHANG_YI_ZHANG, // 上一张 -> Pre
    CMD_FA_SONG,        // 发送 -> Send
    CMD_LIAN_JIE_WIFI,  // 网络 -> WiFi
    CMD_GUAN_BI_SHENG_YIN, // 关闭声音
    CMD_DA_KAI_SHENG_YIN,  // 打开声音
    CMD_PAI_ZHAO_SHI_BIE,  // 帮我拍照并识别 -> Capture + Identify
    CMD_DA_KAI_ASSIST,     // 打开助手模式
    CMD_GUAN_BI_ASSIST,    // 关闭助手模式
};

// 语音识别回调类型
typedef std::function<void(VoiceCommand, int64_t)> voice_command_cb_t;

class SpeechRecognizer {
public:
    static SpeechRecognizer* get_instance();

    // 初始化语音识别
    bool init();

    // 启动语音识别
    void start();

    // 停止语音识别
    void stop();

    // 设置命令回调
    void set_command_callback(voice_command_cb_t cb) { m_command_cb = cb; }

    // 获取命令的中文名称
    static const char* get_command_name(VoiceCommand cmd);

private:
    SpeechRecognizer();
    ~SpeechRecognizer();

    // 禁止拷贝
    SpeechRecognizer(const SpeechRecognizer&) = delete;
    SpeechRecognizer& operator=(const SpeechRecognizer&) = delete;

    // 执行语音命令
    void execute_command(VoiceCommand cmd, int64_t sr_time_ms);

    // 音频输入任务
    static void feed_task(void* arg);

    // 检测任务
    static void detect_task(void* arg);

    // 成员变量
    bool m_initialized;
    bool m_running;
    voice_command_cb_t m_command_cb;
    void* m_afe_handle;
    void* m_afe_data;
    void* m_mic_codec;
    void* m_sr_models;
    volatile int m_task_flag;
};

} // namespace speech
} // namespace who
