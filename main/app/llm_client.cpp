#include "llm_client.hpp"
#include "esp_log.h"
#include "esp_crt_bundle.h"
#include <cstring>
#include <cstdlib>

namespace who {
namespace utils {

// Base64编码表
static const char base64_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const uint8_t* data, size_t len) {
    std::string result;
    result.reserve(((len + 2) / 3) * 4);

    for (size_t i = 0; i < len; i += 3) {
        uint32_t octet_a = i < len ? data[i] : 0;
        uint32_t octet_b = i + 1 < len ? data[i + 1] : 0;
        uint32_t octet_c = i + 2 < len ? data[i + 2] : 0;

        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;

        result += base64_chars[(triple >> 18) & 0x3F];
        result += base64_chars[(triple >> 12) & 0x3F];
        result += (i + 1 < len) ? base64_chars[(triple >> 6) & 0x3F] : '=';
        result += (i + 2 < len) ? base64_chars[triple & 0x3F] : '=';
    }

    return result;
}

} // namespace utils
} // namespace who

namespace who {
namespace llm {

// API 配置
static constexpr const char* LLM_API_URL = "https://api.xiaomimimo.com/v1/chat/completions";
static constexpr const char* LLM_API_KEY = "sk-c2fyz9hrtcylmpv6x4czo6oh7w3ukyv2pucb5gtj1foljznc";
static constexpr const char* LLM_MODEL = "mimo-v2.5";
static constexpr const char* LLM_VISION_MODEL = "mimo-v2.5";

// 系统提示词
static constexpr const char* SYSTEM_PROMPT_LANGUAGE =
    "你是一个专业科普助手，请根据物体名称，提供专业的科普知识150~200字通俗介绍：动物写特征、习性、栖息地；植物写外形、环境、用处；文物写年代、功用、文化价值；日用品写功能与使用场景。格式：名称\n+介绍，多个物体使用换行间隔（回答纯文本，禁用markdown和emoji）。";

static constexpr const char* SYSTEM_PROMPT_AICHAT =
    "请结合上面的一段物体介绍，继续回答用户问题（150~200字，禁用markdown和emoji）；";

static constexpr const char* SYSTEM_PROMPT_LANGUAGE_JSON =
    "请以JSON格式返回：{\"species\":[{\"name\":\"物种名称\",\"intro\":\"150~200字科普介绍\"}]}\n"
    "动物写特征、习性、栖息地；植物写外形、环境、用处；文物写年代、功用、文化价值；日用品写功能与使用场景。\n"
    "禁用markdown和emoji。";

LLMClient* LLMClient::get_instance()
{
    static LLMClient instance;
    return &instance;
}

LLMClient::LLMClient() :
    m_initialized(false),
    m_busy(false),
    m_mode(LLMMode::LANGUAGE),
    m_language_model(LLM_MODEL),
    m_vision_model(LLM_VISION_MODEL),
    m_detection_context(""),
    m_last_error(""),
    m_response_buffer(nullptr),
    m_response_buffer_len(0),
    m_response_buffer_pos(0)
{
}

LLMClient::~LLMClient()
{
    if (m_response_buffer) {
        free(m_response_buffer);
        m_response_buffer = nullptr;
    }
}

bool LLMClient::init()
{
    if (m_initialized) {
        return true;
    }

    // 分配响应缓冲区（使用 PSRAM，初始 16KB）
    m_response_buffer_len = 16 * 1024;
    m_response_buffer = (char*)heap_caps_malloc(m_response_buffer_len, MALLOC_CAP_SPIRAM);
    if (!m_response_buffer) {
        m_response_buffer = (char*)malloc(m_response_buffer_len);
        if (!m_response_buffer) {
            ESP_LOGE(TAG, "Failed to allocate response buffer");
            return false;
        }
    }

    m_initialized = true;
    return true;
}

std::string LLMClient::build_request_json(const char* message)
{
    // 构建 JSON 请求
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", m_language_model.c_str());
    cJSON_AddBoolToObject(root, "stream", false);

    // 消息数组
    cJSON* messages = cJSON_CreateArray();

    if (m_mode == LLMMode::AICHAT) {
        // AIChat 模式：system 消息 + Start 检测内容 + 用户当前问题
        cJSON* sys_msg = cJSON_CreateObject();
        cJSON_AddStringToObject(sys_msg, "role", "system");
        cJSON_AddStringToObject(sys_msg, "content", SYSTEM_PROMPT_AICHAT);
        cJSON_AddItemToArray(messages, sys_msg);

        if (!m_detection_context.empty()) {
            cJSON* ctx_msg = cJSON_CreateObject();
            cJSON_AddStringToObject(ctx_msg, "role", "user");
            cJSON_AddStringToObject(ctx_msg, "content", m_detection_context.c_str());
            cJSON_AddItemToArray(messages, ctx_msg);
        }

        cJSON* user_msg = cJSON_CreateObject();
        cJSON_AddStringToObject(user_msg, "role", "user");
        cJSON_AddStringToObject(user_msg, "content", message);
        cJSON_AddItemToArray(messages, user_msg);
    } else {
        // Language 模式：合并到一条 user 消息
        cJSON* user_msg = cJSON_CreateObject();
        cJSON_AddStringToObject(user_msg, "role", "user");
        std::string merged = std::string(SYSTEM_PROMPT_LANGUAGE);
        if (!m_detection_context.empty()) {
            merged += "\n\n检测到的物体：\n" + m_detection_context;
        }
        merged += "\n\n";
        merged += message;
        cJSON_AddStringToObject(user_msg, "content", merged.c_str());
        cJSON_AddItemToArray(messages, user_msg);
    }

    cJSON_AddItemToObject(root, "messages", messages);

    // 转换为字符串
    char* json_str = cJSON_PrintUnformatted(root);
    std::string result(json_str);
    free(json_str);
    cJSON_Delete(root);

    return result;
}

std::string LLMClient::execute_http_request(const char* request_json, int timeout_ms, int buffer_size)
{
    if (!m_initialized) {
        ESP_LOGE(TAG, "LLMClient not initialized");
        m_last_error = "Not initialized";
        return "";
    }

    if (m_busy) {
        ESP_LOGW(TAG, "LLMClient is busy");
        m_last_error = "Busy";
        return "";
    }

    m_busy = true;
    m_current_response.clear();
    m_last_error.clear();
    m_response_buffer_pos = 0;

    // 配置 HTTP 客户端
    esp_http_client_config_t config = {};
    config.url = LLM_API_URL;
    config.method = HTTP_METHOD_POST;
    config.event_handler = http_event_handler;
    config.user_data = this;
    config.timeout_ms = timeout_ms;
    config.buffer_size = buffer_size;
    config.transport_type = HTTP_TRANSPORT_OVER_SSL;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        m_last_error = "HTTP client init failed";
        m_busy = false;
        return "";
    }

    // 设置请求头
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", ("Bearer " + std::string(LLM_API_KEY)).c_str());
    esp_http_client_set_header(client, "Accept", "application/json");

    // 设置请求体
    esp_http_client_set_post_field(client, request_json, strlen(request_json));

    // 执行请求
    int64_t t_http_start = esp_timer_get_time();
    esp_err_t err = esp_http_client_perform(client);
    int64_t t_http_end = esp_timer_get_time();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[PERF] HTTP请求失败: %s, 耗时 %lld ms", esp_err_to_name(err), (t_http_end - t_http_start) / 1000);
        m_last_error = esp_err_to_name(err);
        esp_http_client_cleanup(client);
        m_busy = false;
        return "";
    }

    // 获取状态码
    int status_code = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "[PERF] LLM响应: HTTP %d, 耗时 %lld ms, 内容长度 %lld",
             status_code, (t_http_end - t_http_start) / 1000,
             esp_http_client_get_content_length(client));

    esp_http_client_cleanup(client);

    m_busy = false;

    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP error %d, response: %.500s", status_code,
                 m_response_buffer ? m_response_buffer : "");
        m_last_error = "HTTP status " + std::to_string(status_code);
        return "";
    }

    return m_current_response;
}

esp_err_t LLMClient::http_event_handler(esp_http_client_event_t *evt)
{
    LLMClient* client = static_cast<LLMClient*>(evt->user_data);

    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            // 累积响应数据
            if (client->m_response_buffer_pos + evt->data_len >= client->m_response_buffer_len) {
                // 需要扩展缓冲区
                int new_len = client->m_response_buffer_len * 2;
                char* new_buf = (char*)realloc(client->m_response_buffer, new_len);
                if (!new_buf) {
                    ESP_LOGE(TAG, "Failed to reallocate response buffer");
                    return ESP_FAIL;
                }
                client->m_response_buffer = new_buf;
                client->m_response_buffer_len = new_len;
            }
            memcpy(client->m_response_buffer + client->m_response_buffer_pos,
                   evt->data, evt->data_len);
            client->m_response_buffer_pos += evt->data_len;
            client->m_response_buffer[client->m_response_buffer_pos] = '\0';
            break;

        case HTTP_EVENT_ON_FINISH:
            // 解析非流式 JSON 响应
            if (client->m_response_buffer && client->m_response_buffer_pos > 0) {
                cJSON* root = cJSON_Parse(client->m_response_buffer);
                if (root) {
                    cJSON* choices = cJSON_GetObjectItem(root, "choices");
                    if (choices && cJSON_IsArray(choices)) {
                        cJSON* choice = cJSON_GetArrayItem(choices, 0);
                        if (choice) {
                            cJSON* message = cJSON_GetObjectItem(choice, "message");
                            if (message) {
                                cJSON* content = cJSON_GetObjectItem(message, "content");
                                if (content && cJSON_IsString(content)) {
                                    client->m_current_response = cJSON_GetStringValue(content);
                                }
                            }
                        }
                    }
                    cJSON_Delete(root);
                } else {
                    ESP_LOGW(TAG, "Failed to parse response JSON");
                }
            }
            // 重置缓冲区
            client->m_response_buffer_pos = 0;
            if (client->m_response_buffer) {
                client->m_response_buffer[0] = '\0';
            }
            break;

        case HTTP_EVENT_ERROR:
            ESP_LOGE(TAG, "HTTP error");
            client->m_last_error = "HTTP error";
            break;

        default:
            break;
    }

    return ESP_OK;
}

std::string LLMClient::send_message_sync(const char* message)
{
    std::string request_json = build_request_json(message);
    return execute_http_request(request_json.c_str(), 60000, 4096);
}

std::map<std::string, std::string> LLMClient::generate_science_intro_batch(const std::vector<std::string>& species_names)
{
    std::map<std::string, std::string> result;

    if (species_names.empty()) {
        ESP_LOGW(TAG, "Empty species list");
        return result;
    }

    // 构建物种列表字符串
    std::string species_list;
    for (size_t i = 0; i < species_names.size(); i++) {
        species_list += species_names[i];
        if (i < species_names.size() - 1) {
            species_list += "、";
        }
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", m_language_model.c_str());
    cJSON_AddBoolToObject(root, "stream", false);

    cJSON* resp_fmt = cJSON_CreateObject();
    cJSON_AddStringToObject(resp_fmt, "type", "json_object");
    cJSON_AddItemToObject(root, "response_format", resp_fmt);

    cJSON* messages = cJSON_CreateArray();

    cJSON* sys_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(sys_msg, "role", "system");
    cJSON_AddStringToObject(sys_msg, "content", SYSTEM_PROMPT_LANGUAGE_JSON);
    cJSON_AddItemToArray(messages, sys_msg);

    cJSON* user_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(user_msg, "role", "user");
    std::string prompt = "请为以下物种生成科普介绍：" + species_list;
    cJSON_AddStringToObject(user_msg, "content", prompt.c_str());
    cJSON_AddItemToArray(messages, user_msg);

    cJSON_AddItemToObject(root, "messages", messages);

    char* json_str = cJSON_PrintUnformatted(root);
    std::string request_json(json_str);
    free(json_str);
    cJSON_Delete(root);

    std::string response = execute_http_request(request_json.c_str(), 120000, 8192);
    if (response.empty()) {
        return result;
    }

    // 解析 JSON 响应
    cJSON* json_root = cJSON_Parse(response.c_str());
    if (json_root) {
        cJSON* species_arr = cJSON_GetObjectItem(json_root, "species");
        if (species_arr && cJSON_IsArray(species_arr)) {
            int count = cJSON_GetArraySize(species_arr);
            for (int i = 0; i < count; i++) {
                cJSON* item = cJSON_GetArrayItem(species_arr, i);
                cJSON* name = cJSON_GetObjectItem(item, "name");
                cJSON* intro = cJSON_GetObjectItem(item, "intro");
                if (name && cJSON_IsString(name) && intro && cJSON_IsString(intro)) {
                    result[name->valuestring] = intro->valuestring;
                }
            }
        }
        cJSON_Delete(json_root);
    }

    // Fallback: JSON 解析失败时尝试旧的 \n\n 格式
    if (result.empty()) {
        ESP_LOGW(TAG, "JSON parse failed, falling back to text parsing");
        std::vector<std::string> sections;
        size_t sec_start = 0;
        while (sec_start < response.length()) {
            size_t sec_end = response.find("\n\n", sec_start);
            if (sec_end == std::string::npos) {
                sections.push_back(response.substr(sec_start));
                break;
            }
            sections.push_back(response.substr(sec_start, sec_end - sec_start));
            sec_start = sec_end + 2;
        }
        for (size_t i = 0; i < species_names.size() && i < sections.size(); i++) {
            std::string& section = sections[i];
            size_t nl = section.find('\n');
            std::string intro = (nl != std::string::npos) ? section.substr(nl + 1) : section;
            while (!intro.empty() && (intro.back() == '\n' || intro.back() == ' ' || intro.back() == '\r')) {
                intro.pop_back();
            }
            if (!intro.empty()) {
                result[species_names[i]] = intro;
            }
        }
    }

    for (const auto& species : species_names) {
        if (result.find(species) == result.end()) {
            result[species] = "暂无科普信息";
            ESP_LOGW(TAG, "No intro found for species: %s", species.c_str());
        }
    }

    return result;
}

std::string LLMClient::send_vision_request(const char* image_base64, const std::vector<std::string>& yolo_results)
{
    // 构建物种列表字符串
    std::string species_list;
    for (size_t i = 0; i < yolo_results.size(); i++) {
        species_list += yolo_results[i];
        if (i < yolo_results.size() - 1) {
            species_list += "、";
        }
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", m_vision_model.c_str());
    cJSON_AddBoolToObject(root, "stream", false);
    cJSON_AddNumberToObject(root, "max_completion_tokens", 2048);

    cJSON* response_format = cJSON_CreateObject();
    cJSON_AddStringToObject(response_format, "type", "json_object");
    cJSON_AddItemToObject(root, "response_format", response_format);

    cJSON* messages = cJSON_CreateArray();

    cJSON* user_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(user_msg, "role", "user");

    cJSON* content_array = cJSON_CreateArray();

    cJSON* img_content = cJSON_CreateObject();
    cJSON_AddStringToObject(img_content, "type", "image_url");

    cJSON* image_url = cJSON_CreateObject();
    std::string data_uri = "data:image/jpeg;base64," + std::string(image_base64);
    cJSON_AddStringToObject(image_url, "url", data_uri.c_str());

    cJSON_AddItemToObject(img_content, "image_url", image_url);
    cJSON_AddItemToArray(content_array, img_content);

    cJSON* text_content = cJSON_CreateObject();
    cJSON_AddStringToObject(text_content, "type", "text");
    std::string prompt;
    if (yolo_results.empty()) {
        prompt = "请以JSON格式返回：{\"species\":[{\"name\":\"物种汉字名\",\"intro\":\"130-180字科普介绍\"}]}\n"
                 "你是一个专业视觉识别助手，请科普图片中的物体并给出简要介绍。\n"
                 "动物写特征、习性、栖息地；植物写外形、环境、用处；文物写年代、功用、文化价值；日用品写功能与使用场景。\n"
                 "禁用markdown和emoji。";
    } else {
        prompt = "请以JSON格式返回：{\"species\":[{\"name\":\"物种汉字名\",\"intro\":\"160-180字科普介绍\"}]}\n"
                 "你是一个专业视觉科普助手，请结合图片更加详细科普下列物体：\n" + species_list + "\n"
                 "动物：特征+习性+栖息地；植物：外形+环境+用途；文物：年代+功用+文化；日用品：功能+使用场景。\n"
                 "禁用markdown和emoji。";
    }
    cJSON_AddStringToObject(text_content, "text", prompt.c_str());
    cJSON_AddItemToArray(content_array, text_content);

    cJSON_AddItemToObject(user_msg, "content", content_array);
    cJSON_AddItemToArray(messages, user_msg);

    cJSON_AddItemToObject(root, "messages", messages);

    char* json_str = cJSON_PrintUnformatted(root);
    std::string request_json(json_str);
    free(json_str);
    cJSON_Delete(root);

    std::string result = execute_http_request(request_json.c_str(), 120000, 8192);
    return result;
}

} // namespace llm
} // namespace who
