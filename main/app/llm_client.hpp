#pragma once

#include "esp_http_client.h"
#include "cJSON.h"
#include <functional>
#include <map>
#include <string>
#include <vector>
#include <cstdint>

namespace who {
namespace utils {

// Base64 编码函数声明
std::string base64_encode(const uint8_t* data, size_t len);

} // namespace utils

namespace llm {

// LLM 模式
enum class LLMMode {
    LANGUAGE,   // 纯文本对话
    VISION,     // 图像理解（带检测结果的上下文）
    AICHAT      // 基于检测结果的上下文聊天
};

class LLMClient {
public:
    static LLMClient* get_instance();

    // 初始化 LLM 客户端
    bool init();

    // 发送消息并获取响应
    std::string send_message_sync(const char* message);

    // 批量生成物种科普介绍
    std::map<std::string, std::string> generate_science_intro_batch(const std::vector<std::string>& species_names);

    // 视觉识别请求- Vision模式使用
    std::string send_vision_request(const char* image_base64, const std::vector<std::string>& yolo_results);

    // 设置 LLM 模式
    void set_mode(LLMMode mode) { m_mode = mode; }
    LLMMode get_mode() const { return m_mode; }

    // 设置检测结果上下文（Vision 模式使用）
    void set_detection_context(const char* context) {
        if (context) {
            m_detection_context = context;
        }
    }

    // 检查是否正在处理请求
    bool is_busy() const { return m_busy; }

    // 设置语言/视觉模型
    void set_language_model(const char* model) { if (model) m_language_model = model; }
    void set_vision_model(const char* model) { if (model) m_vision_model = model; }

    // 获取最后的错误信息
    const char* get_last_error() const { return m_last_error.c_str(); }

private:
    LLMClient();
    ~LLMClient();

    LLMClient(const LLMClient&) = delete;
    LLMClient& operator=(const LLMClient&) = delete;

    // HTTP 事件处理
    static esp_err_t http_event_handler(esp_http_client_event_t *evt);

    // 构建请求 JSON
    std::string build_request_json(const char* message);

    // 通用 HTTP 请求执行
    std::string execute_http_request(const char* request_json, int timeout_ms, int buffer_size);

    static constexpr const char* TAG = "LLMClient";

    bool m_initialized;
    bool m_busy;
    LLMMode m_mode;
    std::string m_language_model;
    std::string m_vision_model;
    std::string m_detection_context;
    std::string m_last_error;

    // 当前响应
    std::string m_current_response;

    // HTTP 响应缓冲
    char* m_response_buffer;
    int m_response_buffer_len;
    int m_response_buffer_pos;

};

} // namespace llm
} // namespace who
