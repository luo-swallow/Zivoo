#include "index.hpp"
#include "sys.hpp"
#include "esp_timer.h"
#include "model/image_process.hpp"
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <string.h>
#include <algorithm>
#include <map>
#include <vector>

// LLM 工作线程 + 消息队列（PSRAM 栈 12KB）
namespace who {
namespace ui {

QueueHandle_t s_llm_queue = nullptr;

static void llm_worker_task(void* arg)
{
    LLMTaskMsg msg;
    while (1) {
        if (xQueueReceive(s_llm_queue, &msg, portMAX_DELAY) == pdTRUE) {
            msg.func(msg.params);
        }
    }
}

void llm_worker_init()
{
    if (s_llm_queue) return;
    s_llm_queue = xQueueCreate(4, sizeof(LLMTaskMsg));
    if (s_llm_queue) {
        xTaskCreatePinnedToCoreWithCaps(llm_worker_task, "llm_worker", 16 * 1024, nullptr, 6, nullptr, 1, MALLOC_CAP_SPIRAM);
    }
}

void llm_worker_send(LLMTaskMsg* msg)
{
    if (s_llm_queue) {
        xQueueSend(s_llm_queue, msg, portMAX_DELAY);
    }
}
} // namespace ui
} // namespace who

// 声明自定义中文字体
LV_FONT_DECLARE(lv_font_alibabapuhuiti_light_16)

// 包含简体拼音字典
#include "lv_pinyin_dict_simplified.h"

static const char *TAG = "MyAppUI";

namespace who {
namespace ui {

// 解析 AI Vision 响应，分离每个物种的名称和介绍
static std::vector<std::pair<std::string, std::string>> parse_vision_species_response(const std::string& response) {
    std::vector<std::pair<std::string, std::string>> result;

    if (response.empty()) {
        return result;
    }

    // 优先尝试 JSON 格式解析
    cJSON* root = cJSON_Parse(response.c_str());
    if (root) {
        cJSON* species_arr = cJSON_GetObjectItem(root, "species");
        if (species_arr && cJSON_IsArray(species_arr)) {
            int count = cJSON_GetArraySize(species_arr);
            for (int i = 0; i < count; i++) {
                cJSON* item = cJSON_GetArrayItem(species_arr, i);
                cJSON* name = cJSON_GetObjectItem(item, "name");
                cJSON* intro = cJSON_GetObjectItem(item, "intro");
                if (name && cJSON_IsString(name) && intro && cJSON_IsString(intro)) {
                    result.push_back({name->valuestring, intro->valuestring});
                }
            }
        }
        cJSON_Delete(root);
        if (!result.empty()) return result;
    }

    // Fallback: 旧的 \n\n 分割格式
    std::vector<std::string> entries;
    size_t pos = 0;
    while (pos < response.size()) {
        size_t next = response.find("\n\n", pos);
        std::string entry;
        if (next == std::string::npos) {
            entry = response.substr(pos);
            pos = response.size();
        } else {
            entry = response.substr(pos, next - pos);
            pos = next + 2;
        }
        while (!entry.empty() && (entry.front() == '\n' || entry.front() == ' ' || entry.front() == '\r')) {
            entry.erase(0, 1);
        }
        while (!entry.empty() && (entry.back() == '\n' || entry.back() == ' ' || entry.back() == '\r')) {
            entry.pop_back();
        }
        if (!entry.empty()) {
            entries.push_back(entry);
        }
    }

    for (const auto& entry : entries) {
        size_t newline_pos = entry.find('\n');
        std::string name;
        std::string desc;
        if (newline_pos == std::string::npos) {
            name = entry;
            desc = "";
        } else {
            name = entry.substr(0, newline_pos);
            desc = entry.substr(newline_pos + 1);
            while (!desc.empty() && (desc.front() == '\n' || desc.front() == ' ' || desc.front() == '\r')) {
                desc.erase(0, 1);
            }
            while (!desc.empty() && (desc.back() == '\n' || desc.back() == ' ' || desc.back() == '\r')) {
                desc.pop_back();
            }
        }
        result.push_back({name, desc});
    }

    return result;
}

MyAppUI::MyAppUI() :
    m_nav_bar(nullptr),
    m_title_label(nullptr),
    m_perf_infer_label(nullptr),
    m_perf_sr_label(nullptr),
    m_settings_btn(nullptr),
    m_settings_container(nullptr),
    m_settings_open(false),
    m_wifi_btn(nullptr),
    m_voice_btn(nullptr),
    m_llm_dropdown(nullptr),
    m_settings_overlay(nullptr),
    m_voice_enabled(true),
    m_assist_btn(nullptr),
    m_assist_enabled(false),
    m_content_area(nullptr),
    m_camera_container(nullptr),
    m_right_container(nullptr),
    m_result_container(nullptr),
    m_result_title_label(nullptr),
    m_table_header(nullptr),
    m_image_canvas(nullptr),
    m_image_buffer(nullptr),
    m_identify_mode(false),
    m_image_manager(nullptr),
    m_frame_cap_node(nullptr),
    m_detect_model(nullptr),
    m_capture_btn(nullptr),
    m_identify_btn(nullptr),
    m_image_count_label(nullptr),
    m_delete_btn(nullptr),
    m_browse_container(nullptr),
    m_pre_btn(nullptr),
    m_start_btn(nullptr),
    m_next_btn(nullptr),
    m_ai_chat_btn(nullptr),
    m_chat_container(nullptr),
    m_chat_input(nullptr),
    m_send_btn(nullptr),
    m_keyboard(nullptr),
    m_ime_pinyin(nullptr),
    m_chat_mode(false),
    m_llm_task_cancelled(false),
    m_camera_pause_cb(nullptr),
    m_camera_resume_cb(nullptr),
    m_detect_seq(0),
    m_assist_capture_pending(false)
{
}

MyAppUI::~MyAppUI()
{
    // LVGL 对象由系统自动清理
    if (m_image_manager) {
        delete m_image_manager;
    }
    if (m_image_buffer) {
        heap_caps_free(m_image_buffer);
    }
}

void MyAppUI::set_frame_cap_node(who::frame_cap::WhoFrameCapNode *node)
{
    m_frame_cap_node = node;
}

void MyAppUI::set_detect_model(dl::detect::Detect *model)
{
    m_detect_model = model;
}

void MyAppUI::init_image_manager()
{
    m_image_manager = new image::ImageManager();
    m_image_manager->init();
}

bool MyAppUI::init()
{
    bsp_display_lock(0);

    // ===========================================
    // 1. 创建顶部导航栏
    // ===========================================
    m_nav_bar = lv_obj_create(lv_scr_act());
    lv_obj_set_size(m_nav_bar, LV_PCT(100), 50);
    lv_obj_align(m_nav_bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(m_nav_bar, lv_color_hex(0xFFB7C5), 0);  // 樱花粉色背景
    lv_obj_set_style_border_width(m_nav_bar, 0, 0);
    lv_obj_set_style_radius(m_nav_bar, 0, 0);
    lv_obj_clear_flag(m_nav_bar, LV_OBJ_FLAG_SCROLLABLE);

    // Flex 布局：横向排列
    lv_obj_set_flex_flow(m_nav_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(m_nav_bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(m_nav_bar, 10, 0);

    // ===========================================
    // 2. AiChat 按钮（居中对齐，初始隐藏）
    // ===========================================
    m_ai_chat_btn = lv_btn_create(lv_scr_act());  // 创建在根屏幕，独立于 flex 布局
    lv_obj_set_size(m_ai_chat_btn, 100, 36);
    lv_obj_set_style_bg_color(m_ai_chat_btn, lv_color_hex(0x00BCD4), 0);  // 青色背景
    lv_obj_set_style_bg_opa(m_ai_chat_btn, LV_OPA_80, 0);
    lv_obj_set_style_radius(m_ai_chat_btn, 8, 0);
    lv_obj_set_style_border_width(m_ai_chat_btn, 0, 0);
    lv_obj_t *ai_chat_label = lv_label_create(m_ai_chat_btn);
    lv_label_set_text(ai_chat_label, "Ai聊天");
    lv_obj_set_style_text_color(ai_chat_label, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(ai_chat_label, &lv_font_alibabapuhuiti_light_16, 0);
    lv_obj_center(ai_chat_label);

    lv_obj_add_event_cb(m_ai_chat_btn, onAiChatClicked, LV_EVENT_CLICKED, this);
    lv_obj_add_flag(m_ai_chat_btn, LV_OBJ_FLAG_HIDDEN);  // 初始隐藏

    // ===========================================
    // 3. 创建导航栏标题 "Zivoo"
    // ===========================================
    m_title_label = lv_label_create(m_nav_bar);
    lv_label_set_text(m_title_label, "Zivoo");
    lv_obj_set_style_text_color(m_title_label, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(m_title_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_pad_left(m_title_label, 50, 0);

    // ===========================================
    // 3.5 性能标签（Zivoo 右侧）
    // ===========================================

    // 推理耗时标签
    m_perf_infer_label = lv_label_create(m_nav_bar);
    lv_label_set_text(m_perf_infer_label, "T:--ms");
    lv_obj_set_style_text_color(m_perf_infer_label, lv_color_hex(0xFFFF00), 0);
    lv_obj_set_style_text_font(m_perf_infer_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_bg_color(m_perf_infer_label, lv_color_hex(0x333333), 0);
    lv_obj_set_style_bg_opa(m_perf_infer_label, LV_OPA_70, 0);
    lv_obj_set_style_pad_left(m_perf_infer_label, 10, 0);
    lv_obj_set_style_pad_right(m_perf_infer_label, 10, 0);
    lv_obj_set_style_pad_top(m_perf_infer_label, 5, 0);
    lv_obj_set_style_pad_bottom(m_perf_infer_label, 5, 0);
    lv_obj_set_style_radius(m_perf_infer_label, 4, 0);

    // 语音识别耗时标签
    m_perf_sr_label = lv_label_create(m_nav_bar);
    lv_label_set_text(m_perf_sr_label, "SR:--ms");
    lv_obj_set_style_text_color(m_perf_sr_label, lv_color_hex(0x00FF88), 0);
    lv_obj_set_style_text_font(m_perf_sr_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_bg_color(m_perf_sr_label, lv_color_hex(0x333333), 0);
    lv_obj_set_style_bg_opa(m_perf_sr_label, LV_OPA_70, 0);
    lv_obj_set_style_pad_left(m_perf_sr_label, 6, 0);
    lv_obj_set_style_pad_right(m_perf_sr_label, 6, 0);
    lv_obj_set_style_pad_top(m_perf_sr_label, 5, 0);
    lv_obj_set_style_pad_bottom(m_perf_sr_label, 5, 0);
    lv_obj_set_style_radius(m_perf_sr_label, 4, 0);

    // 弹性 spacer：将后续按钮推到右侧
    lv_obj_t *nav_spacer = lv_obj_create(m_nav_bar);
    lv_obj_set_flex_grow(nav_spacer, 1);
    lv_obj_set_style_bg_opa(nav_spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(nav_spacer, 0, 0);
    lv_obj_clear_flag(nav_spacer, LV_OBJ_FLAG_SCROLLABLE);

    // ===========================================
    // 4. 在导航栏右侧添加 Capture 和 Identify 按钮（Setting 左侧）
    // ===========================================
    m_capture_btn = lv_btn_create(m_nav_bar);
    lv_obj_set_size(m_capture_btn, 90, 36);
    lv_obj_set_style_bg_color(m_capture_btn, lv_color_hex(0x4CAF50), 0);
    lv_obj_set_style_radius(m_capture_btn, 8, 0);
    lv_obj_set_style_border_width(m_capture_btn, 0, 0);

    lv_obj_t *nav_capture_label = lv_label_create(m_capture_btn);
    lv_label_set_text(nav_capture_label, "拍照");
    lv_obj_set_style_text_color(nav_capture_label, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(nav_capture_label, &lv_font_alibabapuhuiti_light_16, 0);
    lv_obj_center(nav_capture_label);

    lv_obj_add_event_cb(m_capture_btn, onCaptureClicked, LV_EVENT_CLICKED, this);

    m_identify_btn = lv_btn_create(m_nav_bar);
    lv_obj_set_size(m_identify_btn, 90, 36);
    lv_obj_set_style_bg_color(m_identify_btn, lv_color_hex(0x2196F3), 0);
    lv_obj_set_style_radius(m_identify_btn, 8, 0);
    lv_obj_set_style_border_width(m_identify_btn, 0, 0);

    lv_obj_t *nav_identify_label = lv_label_create(m_identify_btn);
    lv_label_set_text(nav_identify_label, "科普界面");
    lv_obj_set_style_text_color(nav_identify_label, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(nav_identify_label, &lv_font_alibabapuhuiti_light_16, 0);
    lv_obj_center(nav_identify_label);

    lv_obj_add_event_cb(m_identify_btn, onIdentifyClicked, LV_EVENT_CLICKED, this);

    // ===========================================
    // 5. 设置按钮（最右侧），点击弹出 WiFi/Voice/Aassist/LLM
    // ===========================================
    m_settings_btn = lv_btn_create(m_nav_bar);
    lv_obj_set_size(m_settings_btn,90, 36);
    lv_obj_set_style_bg_color(m_settings_btn, lv_color_hex(0x607D8B), 0);  // 蓝灰色
    lv_obj_set_style_bg_opa(m_settings_btn, LV_OPA_70, 0);
    lv_obj_set_style_radius(m_settings_btn, 8, 0);
    lv_obj_set_style_border_width(m_settings_btn, 0, 0);

    lv_obj_t *settings_label = lv_label_create(m_settings_btn);
    lv_label_set_text(settings_label, "设置");
    lv_obj_set_style_text_color(settings_label, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(settings_label, &lv_font_alibabapuhuiti_light_16, 0);
    lv_obj_center(settings_label);

    lv_obj_add_event_cb(m_settings_btn, onSettingsClicked, LV_EVENT_CLICKED, this);

    // ===========================================
    // 5. 创建设置弹出容器（WiFi/Voice/Aassist/LLM 水平排列）
    // ===========================================
    m_settings_container = lv_obj_create(lv_scr_act());
    lv_obj_set_size(m_settings_container, 420, 80);
    lv_obj_set_style_bg_color(m_settings_container, lv_color_hex(0x455A64), 0);  // 深灰背景
    lv_obj_set_style_border_width(m_settings_container, 1, 0);
    lv_obj_set_style_border_color(m_settings_container, lv_color_hex(0x607D8B), 0);
    lv_obj_set_style_radius(m_settings_container, 8, 0);
    lv_obj_set_style_pad_all(m_settings_container, 6, 0);
    lv_obj_set_style_pad_column(m_settings_container, 12, 0);
    lv_obj_clear_flag(m_settings_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(m_settings_container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(m_settings_container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(m_settings_container, LV_OBJ_FLAG_HIDDEN);  // 初始隐藏

    // WiFi 按钮
    m_wifi_btn = lv_btn_create(m_settings_container);
    lv_obj_set_size(m_wifi_btn, 80, 36);
    lv_obj_set_style_bg_color(m_wifi_btn, lv_color_hex(0xF44336), 0);
    lv_obj_set_style_bg_opa(m_wifi_btn, LV_OPA_70, 0);
    lv_obj_set_style_radius(m_wifi_btn, 8, 0);
    lv_obj_set_style_border_width(m_wifi_btn, 0, 0);
    lv_obj_t *wifi_label = lv_label_create(m_wifi_btn);
    lv_label_set_text(wifi_label, "WiFi");
    lv_obj_set_style_text_color(wifi_label, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(wifi_label, &lv_font_montserrat_16, 0);
    lv_obj_center(wifi_label);
    lv_obj_add_event_cb(m_wifi_btn, onWifiClicked, LV_EVENT_CLICKED, this);

    // Voice 按钮
    m_voice_btn = lv_btn_create(m_settings_container);
    lv_obj_set_size(m_voice_btn, 80, 36);
    lv_obj_set_style_bg_color(m_voice_btn, lv_color_hex(0x2196F3), 0);
    lv_obj_set_style_bg_opa(m_voice_btn, LV_OPA_70, 0);
    lv_obj_set_style_radius(m_voice_btn, 8, 0);
    lv_obj_set_style_border_width(m_voice_btn, 0, 0);
    lv_obj_t *voice_label = lv_label_create(m_voice_btn);
    lv_label_set_text(voice_label, "关闭声音");
    lv_obj_set_style_text_color(voice_label, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(voice_label, &lv_font_alibabapuhuiti_light_16, 0);
    lv_obj_center(voice_label);
    lv_obj_add_event_cb(m_voice_btn, onVoiceClicked, LV_EVENT_CLICKED, this);

    // Assist 按钮（默认关闭，灰色表示禁用状态）
    m_assist_btn = lv_btn_create(m_settings_container);
    lv_obj_set_size(m_assist_btn, 80, 36);
    lv_obj_set_style_bg_color(m_assist_btn, lv_color_hex(0x9E9E9E), 0);  // 灰色=关闭
    lv_obj_set_style_bg_opa(m_assist_btn, LV_OPA_70, 0);
    lv_obj_set_style_radius(m_assist_btn, 8, 0);
    lv_obj_set_style_border_width(m_assist_btn, 0, 0);
    lv_obj_t *assist_label = lv_label_create(m_assist_btn);
    lv_label_set_text(assist_label, "辅助模式");
    lv_obj_set_style_text_color(assist_label, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(assist_label, &lv_font_alibabapuhuiti_light_16, 0);
    lv_obj_center(assist_label);
    lv_obj_add_event_cb(m_assist_btn, onAssistClicked, LV_EVENT_CLICKED, this);

    // LLM 下拉菜单
    m_llm_dropdown = lv_dropdown_create(m_settings_container);
    lv_obj_set_size(m_llm_dropdown, 110, 36);
    lv_dropdown_set_options(m_llm_dropdown, "语言模型\n视觉模型");
    lv_dropdown_set_text(m_llm_dropdown, "LLM");
    lv_obj_set_style_bg_color(m_llm_dropdown, lv_color_hex(0x9C27B0), 0);
    lv_obj_set_style_bg_opa(m_llm_dropdown, LV_OPA_70, 0);
    lv_obj_set_style_radius(m_llm_dropdown, 8, 0);
    lv_obj_set_style_border_width(m_llm_dropdown, 0, 0);
    lv_obj_set_style_text_color(m_llm_dropdown, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(m_llm_dropdown, &lv_font_montserrat_14, 0);
    lv_obj_t *list = lv_dropdown_get_list(m_llm_dropdown);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x4A148C), 0);
    lv_obj_set_style_text_color(list, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(list, &lv_font_alibabapuhuiti_light_16, 0);
    lv_obj_align_to(list, m_llm_dropdown, LV_ALIGN_OUT_BOTTOM_LEFT, 10, 0);
    lv_obj_add_event_cb(m_llm_dropdown, onLLMDropdownChanged, LV_EVENT_VALUE_CHANGED, this);

    // ===========================================
    // 6. 创建主内容区域
    // ===========================================
    m_content_area = lv_obj_create(lv_scr_act());
    lv_obj_set_size(m_content_area, LV_PCT(100), 550);
    lv_obj_align_to(m_content_area, m_nav_bar, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_radius(m_content_area, 0, 0);
    lv_obj_set_style_bg_color(m_content_area, lv_color_hex(0x9FE2BF), 0);
    lv_obj_set_style_border_width(m_content_area, 2, 0);
    lv_obj_set_style_border_color(m_content_area, lv_color_hex(0x6BBF95), 0);
    lv_obj_set_style_pad_left(m_content_area, 30, 0);
    lv_obj_set_style_pad_right(m_content_area, 30, 0);
    lv_obj_set_style_pad_top(m_content_area, 10, 0);
    lv_obj_set_style_pad_bottom(m_content_area, 10, 0);
    lv_obj_clear_flag(m_content_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(m_content_area, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(m_content_area, 40, 0);
    lv_obj_set_flex_align(m_content_area, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);

    // 左侧摄像头容器 (525x525)
    m_camera_container = lv_obj_create(m_content_area);
    lv_obj_set_size(m_camera_container, CAMERA_CONTAINER_W, CAMERA_CONTAINER_H);
    lv_obj_set_style_bg_color(m_camera_container, lv_color_hex(0xFFA500), 0);
    lv_obj_set_style_border_width(m_camera_container, 3, 0);
    lv_obj_set_style_border_color(m_camera_container, lv_color_hex(0xFEF8FA), 0);
    lv_obj_set_style_radius(m_camera_container, 10, 0);
    lv_obj_clear_flag(m_camera_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(m_camera_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(m_camera_container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // ===========================================
    // 右侧容器（垂直布局，包含结果容器和按钮）
    // ===========================================
    m_right_container = lv_obj_create(m_content_area);
    lv_obj_set_size(m_right_container, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(m_right_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(m_right_container, 0, 0);
    lv_obj_set_style_radius(m_right_container, 0, 0);
    lv_obj_set_style_pad_all(m_right_container, 0, 0);
    lv_obj_clear_flag(m_right_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(m_right_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(m_right_container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(m_right_container, 15, 0);

    // 结果容器（放在右侧容器内）
    m_result_container = lv_obj_create(m_right_container);
    lv_obj_set_size(m_result_container, 400, 420);
    lv_obj_set_style_bg_color(m_result_container, lv_color_hex(0x345B70), 0);
    lv_obj_set_style_border_width(m_result_container, 3, 0);
    lv_obj_set_style_border_color(m_result_container, lv_color_hex(0x008FB2), 0);
    lv_obj_set_style_radius(m_result_container, 10, 0);
    lv_obj_set_flex_flow(m_result_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(m_result_container, LV_FLEX_ALIGN_START,  LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(m_result_container, 10, 0);
    lv_obj_set_style_pad_row(m_result_container, 0, 0);
    lv_obj_set_scrollbar_mode(m_result_container, LV_SCROLLBAR_MODE_AUTO);

    // 标题标签 "识别结果"
    m_result_title_label = lv_label_create(m_result_container);
    lv_label_set_text(m_result_title_label, "识别结果");
    lv_obj_set_style_text_color(m_result_title_label, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(m_result_title_label, &lv_font_alibabapuhuiti_light_16, 0);
    lv_obj_set_style_text_align(m_result_title_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(m_result_title_label, LV_PCT(100));
    lv_obj_set_height(m_result_title_label, 25);

    // 表头行（序号、名称、置信度）
    m_table_header = lv_obj_create(m_result_container);
    lv_obj_t *header_row = m_table_header;
    lv_obj_set_size(header_row, LV_PCT(100), 26);
    lv_obj_set_flex_flow(header_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(header_row, 0, 0);
    lv_obj_set_style_border_width(header_row, 1, 0);
    lv_obj_set_style_border_color(header_row, lv_color_hex(0x3D6B80), 0);
    lv_obj_set_style_radius(header_row, 0, 0);
    lv_obj_set_style_clip_corner(header_row, true, 0);
    lv_obj_set_style_bg_color(header_row, lv_color_hex(0x008FB2), 0);
    lv_obj_set_style_bg_opa(header_row, LV_OPA_60, 0);
    lv_obj_set_style_text_font(header_row, &lv_font_alibabapuhuiti_light_16, 0);
    lv_obj_clear_flag(header_row, LV_OBJ_FLAG_SCROLLABLE);

    // 表头：序号
    lv_obj_t *hdr_seq = lv_label_create(header_row);
    lv_obj_set_size(hdr_seq, 52, LV_PCT(100));
    lv_label_set_text(hdr_seq, "序号");
    lv_obj_set_style_text_align(hdr_seq, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(hdr_seq, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_pad_top(hdr_seq, 4, 0);
    lv_obj_set_style_pad_bottom(hdr_seq, 4, 0);
    lv_obj_set_style_border_width(hdr_seq, 1, LV_STATE_DEFAULT);
    lv_obj_set_style_border_side(hdr_seq, LV_BORDER_SIDE_RIGHT, 0);
    lv_obj_set_style_border_color(hdr_seq, lv_color_hex(0x3D6B80), 0);

    // 表头：名称
    lv_obj_t *hdr_name = lv_label_create(header_row);
    lv_obj_set_height(hdr_name, LV_PCT(100));
    lv_obj_set_flex_grow(hdr_name, 1);
    lv_label_set_text(hdr_name, "名称");
    lv_obj_set_style_text_align(hdr_name, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(hdr_name, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_pad_top(hdr_name, 4, 0);
    lv_obj_set_style_pad_bottom(hdr_name, 4, 0);
    lv_obj_set_style_pad_left(hdr_name, 6, 0);
    lv_obj_set_style_border_width(hdr_name, 1, LV_STATE_DEFAULT);
    lv_obj_set_style_border_side(hdr_name, LV_BORDER_SIDE_RIGHT, 0);
    lv_obj_set_style_border_color(hdr_name, lv_color_hex(0x3D6B80), 0);

    // 表头：置信度
    lv_obj_t *hdr_conf = lv_label_create(header_row);
    lv_obj_set_size(hdr_conf, 54, LV_PCT(100));
    lv_label_set_text(hdr_conf, "置信度");
    lv_obj_set_style_text_align(hdr_conf, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(hdr_conf, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_pad_top(hdr_conf, 4, 0);
    lv_obj_set_style_pad_bottom(hdr_conf, 4, 0);

    // ===========================================
    // 第二行容器：图片计数标签和删除按钮（初始隐藏）
    // ===========================================
    lv_obj_t *count_delete_container = lv_obj_create(m_right_container);
    lv_obj_set_size(count_delete_container, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(count_delete_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(count_delete_container, 0, 0);
    lv_obj_set_style_pad_all(count_delete_container, 0, 0);
    lv_obj_set_flex_flow(count_delete_container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(count_delete_container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(count_delete_container, 15, 0);
    lv_obj_add_flag(count_delete_container, LV_OBJ_FLAG_HIDDEN);  // 初始隐藏

    // 图片计数标签（使用带背景的对象）
    m_image_count_label = lv_obj_create(count_delete_container);
    lv_obj_set_size(m_image_count_label, 60, 36);  // 固定尺寸，与按钮高度一致
    lv_obj_set_style_bg_color(m_image_count_label, lv_color_hex(0xFFB7C5), 0);  // 樱花粉色背景
    lv_obj_set_style_bg_opa(m_image_count_label, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(m_image_count_label, lv_color_hex(0xFF69B4), 0);  // 深粉色边框
    lv_obj_set_style_border_width(m_image_count_label, 2, 0);
    lv_obj_set_style_radius(m_image_count_label, 6, 0);
    lv_obj_set_style_pad_left(m_image_count_label, 8, 0);
    lv_obj_set_style_pad_right(m_image_count_label, 8, 0);
    lv_obj_set_style_pad_top(m_image_count_label, 4, 0);
    lv_obj_set_style_pad_bottom(m_image_count_label, 4, 0);

    // 在容器内创建标签文本
    lv_obj_t *count_text = lv_label_create(m_image_count_label);
    lv_label_set_text(count_text, "0/0");
    lv_obj_set_style_text_color(count_text, lv_color_hex(0x333333), 0);  // 深色文字
    lv_obj_set_style_text_font(count_text, &lv_font_montserrat_14, 0);
    lv_obj_center(count_text);

    // Delete 删除按钮
    m_delete_btn = lv_btn_create(count_delete_container);
    lv_obj_set_size(m_delete_btn, 80, 36);
    lv_obj_set_style_bg_color(m_delete_btn, lv_color_hex(0xE91E63), 0);  // 粉红色
    lv_obj_set_style_radius(m_delete_btn, 8, 0);
    lv_obj_set_style_border_width(m_delete_btn, 0, 0);

    lv_obj_t *delete_label = lv_label_create(m_delete_btn);
    lv_label_set_text(delete_label, "去除照片");
    lv_obj_set_style_text_color(delete_label, lv_color_hex(0xffffff), 0);  // 白色文字
    lv_obj_set_style_text_font(delete_label, &lv_font_alibabapuhuiti_light_16, 0);
    lv_obj_center(delete_label);

    lv_obj_add_event_cb(m_delete_btn, onDeleteClicked, LV_EVENT_CLICKED, this);

    // ===========================================
    // 第三行按钮容器：Pre、Start、Next（初始隐藏）
    // ===========================================

    // 浏览按钮区域（初始隐藏，在右侧容器内）
    m_browse_container = lv_obj_create(m_right_container);
    lv_obj_set_size(m_browse_container, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(m_browse_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(m_browse_container, 0, 0);
    lv_obj_set_style_pad_all(m_browse_container, 0, 0);
    lv_obj_set_flex_flow(m_browse_container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(m_browse_container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(m_browse_container, 15, 0);
    lv_obj_add_flag(m_browse_container, LV_OBJ_FLAG_HIDDEN);  // 初始隐藏

    // Pre 上一张按钮（黄色背景）
    m_pre_btn = lv_btn_create(m_browse_container);
    lv_obj_set_size(m_pre_btn, 80, 36);
    lv_obj_set_style_bg_color(m_pre_btn, lv_color_hex(0xFFEB3B), 0);  // 黄色
    lv_obj_set_style_radius(m_pre_btn, 8, 0);
    lv_obj_set_style_border_width(m_pre_btn, 0, 0);

    lv_obj_t *pre_label = lv_label_create(m_pre_btn);
    lv_label_set_text(pre_label, "上一张");
    lv_obj_set_style_text_color(pre_label, lv_color_hex(0x333333), 0);  // 深色文字
    lv_obj_set_style_text_font(pre_label, &lv_font_alibabapuhuiti_light_16, 0);
    lv_obj_center(pre_label);

    lv_obj_add_event_cb(m_pre_btn, onPreClicked, LV_EVENT_CLICKED, this);

    // Start 开始按钮（红色背景）
    m_start_btn = lv_btn_create(m_browse_container);
    lv_obj_set_size(m_start_btn, 100, 36);
    lv_obj_set_style_bg_color(m_start_btn, lv_color_hex(0xF44336), 0);  // 红色
    lv_obj_set_style_radius(m_start_btn, 8, 0);
    lv_obj_set_style_border_width(m_start_btn, 0, 0);

    lv_obj_t *start_label = lv_label_create(m_start_btn);
    lv_label_set_text(start_label, "启动识别");
    lv_obj_set_style_text_color(start_label, lv_color_hex(0xffffff), 0);  // 白色文字
    lv_obj_set_style_text_font(start_label, &lv_font_alibabapuhuiti_light_16, 0);
    lv_obj_center(start_label);

    lv_obj_add_event_cb(m_start_btn, onStartClicked, LV_EVENT_CLICKED, this);

    // Next 下一张按钮（黄色背景）
    m_next_btn = lv_btn_create(m_browse_container);
    lv_obj_set_size(m_next_btn, 80, 36);
    lv_obj_set_style_bg_color(m_next_btn, lv_color_hex(0xFFEB3B), 0);  // 黄色
    lv_obj_set_style_radius(m_next_btn, 8, 0);
    lv_obj_set_style_border_width(m_next_btn, 0, 0);

    lv_obj_t *next_label = lv_label_create(m_next_btn);
    lv_label_set_text(next_label, "下一张");
    lv_obj_set_style_text_color(next_label, lv_color_hex(0x333333), 0);  // 深色文字
    lv_obj_set_style_text_font(next_label, &lv_font_alibabapuhuiti_light_16, 0);
    lv_obj_center(next_label);

    lv_obj_add_event_cb(m_next_btn, onNextClicked, LV_EVENT_CLICKED, this);

    // ===========================================
    // 聊天输入容器（在右侧容器内，结果下面，初始隐藏）
    // ===========================================
    m_chat_container = lv_obj_create(m_right_container);
    lv_obj_set_size(m_chat_container, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(m_chat_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(m_chat_container, 0, 0);
    lv_obj_set_style_pad_all(m_chat_container, 0, 0);
    lv_obj_set_flex_flow(m_chat_container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(m_chat_container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(m_chat_container, 10, 0);
    lv_obj_add_flag(m_chat_container, LV_OBJ_FLAG_HIDDEN);  // 初始隐藏

    // 文本输入框
    m_chat_input = lv_textarea_create(m_chat_container);
    lv_obj_set_size(m_chat_input, 300, 36);
    lv_textarea_set_placeholder_text(m_chat_input, "输入消息...");
    lv_textarea_set_one_line(m_chat_input, true);
    lv_obj_set_style_bg_color(m_chat_input, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_opa(m_chat_input, LV_OPA_90, 0);
    lv_obj_set_style_radius(m_chat_input, 6, 0);
    lv_obj_set_style_border_width(m_chat_input, 1, 0);
    lv_obj_set_style_border_color(m_chat_input, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_font(m_chat_input, &lv_font_montserrat_14, 0);

    // Send 发送按钮
    m_send_btn = lv_btn_create(m_chat_container);
    lv_obj_set_size(m_send_btn, 80, 36);
    lv_obj_set_style_bg_color(m_send_btn, lv_color_hex(0x2196F3), 0);  // 蓝色
    lv_obj_set_style_radius(m_send_btn, 6, 0);
    lv_obj_set_style_border_width(m_send_btn, 0, 0);

    lv_obj_t *send_label = lv_label_create(m_send_btn);
    lv_label_set_text(send_label, "Send");
    lv_obj_set_style_text_color(send_label, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(send_label, &lv_font_montserrat_14, 0);
    lv_obj_center(send_label);

    lv_obj_add_event_cb(m_send_btn, onSendClicked, LV_EVENT_CLICKED, this);

    // ===========================================
    // 拼音键盘（居左放置，初始隐藏）
    // ===========================================
    m_keyboard = lv_keyboard_create(lv_scr_act());
    lv_keyboard_set_textarea(m_keyboard, m_chat_input);
    lv_obj_set_size(m_keyboard, 598, 200);  // 宽度598，高度200
    lv_obj_add_flag(m_keyboard, LV_OBJ_FLAG_HIDDEN);  // 初始隐藏

    // 设置键盘按钮字体大小
    lv_obj_set_style_text_font(m_keyboard, &lv_font_montserrat_20, LV_PART_ITEMS);

    // 创建拼音输入法
    m_ime_pinyin = lv_ime_pinyin_create(lv_scr_act());
    lv_ime_pinyin_set_keyboard(m_ime_pinyin, m_keyboard);
    lv_ime_pinyin_set_mode(m_ime_pinyin, LV_IME_PINYIN_MODE_K26);  // 26键模式

    // 设置自定义简体拼音字典
    lv_ime_pinyin_set_dict(m_ime_pinyin, (lv_pinyin_dict_t *)lv_ime_pinyin_simplified_dict);

    // 为候选面板设置中文字体和样式
    lv_obj_t *cand_panel = lv_ime_pinyin_get_cand_panel(m_ime_pinyin);
    if (cand_panel) {
        lv_obj_set_style_text_font(cand_panel, &lv_font_alibabapuhuiti_light_16, 0);
        lv_obj_set_width(cand_panel, 598);  // 候选面板宽度与键盘一致
        lv_obj_set_style_bg_color(cand_panel, lv_color_hex(0xffffff), 0);  // 白色背景
        lv_obj_set_style_bg_opa(cand_panel, LV_OPA_COVER, 0);  // 完全不透明
        lv_obj_set_style_text_color(cand_panel, lv_color_hex(0x333333), 0);  // 深色文字
        // 设置候选面板按钮的样式
        lv_obj_set_style_bg_color(cand_panel, lv_color_hex(0xffffff), LV_PART_ITEMS);
        lv_obj_set_style_text_color(cand_panel, lv_color_hex(0x333333), LV_PART_ITEMS);
        lv_obj_add_flag(cand_panel, LV_OBJ_FLAG_HIDDEN);  // 初始隐藏
    }

    // 键盘位置：居左紧靠底部
    lv_obj_align(m_keyboard, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    // 为输入框设置中文字体（支持中文显示）
    lv_obj_set_style_text_font(m_chat_input, &lv_font_alibabapuhuiti_light_16, 0);

    // 为输入框添加点击事件：点击输入框显示键盘和候选面板
    lv_obj_add_event_cb(m_chat_input, [](lv_event_t *e) {
        MyAppUI *app = (MyAppUI*)lv_event_get_user_data(e);
        if (!app || !app->m_keyboard) return;
        lv_obj_clear_flag(app->m_keyboard, LV_OBJ_FLAG_HIDDEN);
        if (app->m_ime_pinyin) {
            lv_obj_t *panel = lv_ime_pinyin_get_cand_panel(app->m_ime_pinyin);
            if (panel) lv_obj_clear_flag(panel, LV_OBJ_FLAG_HIDDEN);
        }
    }, LV_EVENT_CLICKED, this);

    // 为主要区域添加点击事件：点击时隐藏键盘和候选面板
    lv_obj_add_event_cb(m_content_area, [](lv_event_t *e) {
        MyAppUI *app = (MyAppUI*)lv_event_get_user_data(e);
        if (!app || !app->m_keyboard) return;
        lv_obj_add_flag(app->m_keyboard, LV_OBJ_FLAG_HIDDEN);
        if (app->m_ime_pinyin) {
            lv_obj_t *panel = lv_ime_pinyin_get_cand_panel(app->m_ime_pinyin);
            if (panel) lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
        }
    }, LV_EVENT_CLICKED, this);

    lv_obj_add_event_cb(m_camera_container, [](lv_event_t *e) {
        MyAppUI *app = (MyAppUI*)lv_event_get_user_data(e);
        if (!app || !app->m_keyboard) return;
        lv_obj_add_flag(app->m_keyboard, LV_OBJ_FLAG_HIDDEN);
        if (app->m_ime_pinyin) {
            lv_obj_t *panel = lv_ime_pinyin_get_cand_panel(app->m_ime_pinyin);
            if (panel) lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
        }
    }, LV_EVENT_CLICKED, this);

    lv_obj_add_event_cb(m_result_container, [](lv_event_t *e) {
        MyAppUI *app = (MyAppUI*)lv_event_get_user_data(e);
        if (!app || !app->m_keyboard) return;
        lv_obj_add_flag(app->m_keyboard, LV_OBJ_FLAG_HIDDEN);
        if (app->m_ime_pinyin) {
            lv_obj_t *panel = lv_ime_pinyin_get_cand_panel(app->m_ime_pinyin);
            if (panel) lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
        }
    }, LV_EVENT_CLICKED, this);

    lv_obj_add_event_cb(m_nav_bar, [](lv_event_t *e) {
        MyAppUI *app = (MyAppUI*)lv_event_get_user_data(e);
        if (!app || !app->m_keyboard) return;
        lv_obj_add_flag(app->m_keyboard, LV_OBJ_FLAG_HIDDEN);
        if (app->m_ime_pinyin) {
            lv_obj_t *panel = lv_ime_pinyin_get_cand_panel(app->m_ime_pinyin);
            if (panel) lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
        }
    }, LV_EVENT_CLICKED, this);

    bsp_display_unlock();

    return true;
}

lv_obj_t *MyAppUI::get_camera_container()
{
    return m_camera_container;
}

lv_obj_t *MyAppUI::get_result_container()
{
    return m_result_container;
}

void MyAppUI::update_detect_result(int count, const char *info)
{
    if (m_result_title_label == nullptr) {
        return;
    }

    bsp_display_lock(0);

    lv_obj_set_style_text_font(m_result_title_label, &lv_font_alibabapuhuiti_light_16, 0);

    if (count <= 0 || !info) {
        lv_label_set_text(m_result_title_label, "识别结果");
        bsp_display_unlock();
        return;
    }

    // 解析 info 字符串，提取每个检测目标的类别和置信度
    std::string info_str(info);
    struct DetInfo { int category; float score; };
    std::vector<DetInfo> detections;

    size_t pos = 0;
    while (pos < info_str.size()) {
        size_t cat_pos = info_str.find("Cat:", pos);
        size_t score_pos = info_str.find("Score:", pos);
        if (cat_pos == std::string::npos || score_pos == std::string::npos) break;

        int cat_val = 0;
        float score_val = 0;
        sscanf(info_str.c_str() + cat_pos, "Cat: %d", &cat_val);
        sscanf(info_str.c_str() + score_pos, "Score: %f", &score_val);

        detections.push_back({cat_val, score_val});

        size_t next_obj = info_str.find("Object ", score_pos + 7);
        if (next_obj == std::string::npos) break;
        pos = next_obj;
    }

    // 每个检测目标单独一行：三列标签（序号、物种名称、置信度）占满整行
    for (size_t i = 0; i < detections.size(); i++) {
        m_detect_seq++;
        const auto &det = detections[i];

        // 行容器（水平 flex 布局，显示边框）
        lv_obj_t *row = lv_obj_create(m_result_container);
        lv_obj_set_size(row, LV_PCT(100), 26);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(0x3D6B80), 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_clip_corner(row, true, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_0, 0);

        // 交替行背景
        if ((m_detect_history.size() + i) % 2 == 0) {
            lv_obj_set_style_bg_color(row, lv_color_hex(0x2E5A6E), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_40, 0);
        }

        // 序号标签（固定 52px，文字居中，垂直填充整行）
        char seq_buf[16];
        snprintf(seq_buf, sizeof(seq_buf), "%03lu", (unsigned long)m_detect_seq);
        lv_obj_t *seq_label = lv_label_create(row);
        lv_obj_set_size(seq_label, 52, LV_PCT(100));
        lv_obj_set_style_pad_top(seq_label, 4, 0);
        lv_obj_set_style_pad_bottom(seq_label, 4, 0);
        lv_label_set_text(seq_label, seq_buf);
        lv_obj_set_style_text_align(seq_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(seq_label, lv_color_hex(0xFFA726), 0);
        lv_obj_set_style_text_font(seq_label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_border_width(seq_label, 1, LV_STATE_DEFAULT);
        lv_obj_set_style_border_side(seq_label, LV_BORDER_SIDE_RIGHT, 0);
        lv_obj_set_style_border_color(seq_label, lv_color_hex(0x3D6B80), 0);

        // 物种名称标签（自适应填充剩余宽度，左对齐，垂直填充整行）
        lv_obj_t *name_label = lv_label_create(row);
        lv_obj_set_height(name_label, LV_PCT(100));
        lv_obj_set_flex_grow(name_label, 1);
        lv_obj_set_style_pad_top(name_label, 4, 0);
        lv_obj_set_style_pad_bottom(name_label, 4, 0);
        lv_obj_set_style_pad_left(name_label, 6, 0);
        lv_label_set_text(name_label, coco_classes[det.category]);
        lv_obj_set_style_text_align(name_label, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_style_text_color(name_label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(name_label, &lv_font_alibabapuhuiti_light_16, 0);
        lv_obj_set_style_border_width(name_label, 1, LV_STATE_DEFAULT);
        lv_obj_set_style_border_side(name_label, LV_BORDER_SIDE_RIGHT, 0);
        lv_obj_set_style_border_color(name_label, lv_color_hex(0x3D6B80), 0);

        // 置信度标签（固定 54px，文字居中，垂直填充整行）
        int pct = (int)(det.score * 100);
        char conf_buf[16];
        snprintf(conf_buf, sizeof(conf_buf), "%d%%", pct);
        lv_obj_t *conf_label = lv_label_create(row);
        lv_obj_set_size(conf_label, 54, LV_PCT(100));
        lv_obj_set_style_pad_top(conf_label, 4, 0);
        lv_obj_set_style_pad_bottom(conf_label, 4, 0);
        lv_label_set_text(conf_label, conf_buf);
        lv_obj_set_style_text_align(conf_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(conf_label, &lv_font_montserrat_14, 0);

        // 置信度颜色：>=80% 绿色，60-80% 黄色，40-60% 橙色，<40% 红色
        uint32_t conf_color;
        if (pct >= 80) conf_color = 0x4CAF50;
        else if (pct >= 60) conf_color = 0xFFC107;
        else if (pct >= 40) conf_color = 0xFF9800;
        else conf_color = 0xF44336;
        lv_obj_set_style_text_color(conf_label, lv_color_hex(conf_color), 0);

        m_detect_history.push_back(row);
    }

    // 限制历史记录数量，删除最旧的行
    while (m_detect_history.size() > DETECT_HISTORY_MAX) {
        lv_obj_delete(m_detect_history.front());
        m_detect_history.erase(m_detect_history.begin());
    }

    // 自动滚动到底部显示最新结果
    lv_obj_scroll_to_y(m_result_container, LV_COORD_MAX, LV_ANIM_OFF);

    bsp_display_unlock();
}


void MyAppUI::switch_to_identify_mode()
{
    if (m_identify_mode) return;

    m_identify_mode = true;

    // 清除实时检测历史数据（切换到 identify 模式时清空）
    clear_result_textareas();

    // 暂停摄像头和检测流水线
    if (m_camera_pause_cb) {
        m_camera_pause_cb();
        }

    bsp_display_lock(0);

    // 隐藏表头行
    if (m_table_header) lv_obj_add_flag(m_table_header, LV_OBJ_FLAG_HIDDEN);

    // identify/AIchat 模式增加结果容器的行间距
    lv_obj_set_style_pad_row(m_result_container, 8, 0);

    // identify 模式使用中文字体
    lv_obj_set_style_text_font(m_result_title_label, &lv_font_alibabapuhuiti_light_16, 0);
    lv_label_set_text(m_result_title_label, "识别结果");

    // 禁用Capture按钮（identify模式下不可用）
    lv_obj_add_state(m_capture_btn, LV_STATE_DISABLED);
    lv_obj_set_style_bg_color(m_capture_btn, lv_color_hex(0x388E3C), LV_STATE_DISABLED);  // 深绿色表示禁用

    // 创建图片 canvas
    if (!m_image_canvas) {
        create_image_canvas();
    }

    // 隐藏摄像头容器内除m_image_canvas外的canvas
    lv_obj_t *child0 = lv_obj_get_child(m_camera_container, 0);
    lv_obj_t *child1 = lv_obj_get_child(m_camera_container, 1);

    if (child0 && child0 != m_image_canvas) {
        // 第一个子对象是摄像头canvas，隐藏它
        lv_obj_add_flag(child0, LV_OBJ_FLAG_HIDDEN);
    } else if (child1 && child1 != m_image_canvas) {
        // 第二个子对象是摄像头canvas，隐藏它
        lv_obj_add_flag(child1, LV_OBJ_FLAG_HIDDEN);
    }

    // 显示图片canvas
    if (m_image_canvas) {
        lv_obj_clear_flag(m_image_canvas, LV_OBJ_FLAG_HIDDEN);
    }

    // 显示图片计数标签和删除按钮的父容器
    lv_obj_t *count_delete_container = lv_obj_get_parent(m_image_count_label);
    if (count_delete_container) {
        lv_obj_clear_flag(count_delete_container, LV_OBJ_FLAG_HIDDEN);
    }
    update_image_count_label();

    // 更新图片显示
    update_image_display();

    bsp_display_unlock();
}

void MyAppUI::switch_to_camera_mode()
{
    if (!m_identify_mode) return;

    m_identify_mode = false;

    // 重置 Assist 捕获等待标志（用户手动切回摄像头模式时）
    m_assist_capture_pending = false;

    // camera 模式恢复紧凑的行间距
    lv_obj_set_style_pad_row(m_result_container, 0, 0);

    // 清除旧的识别结果 textarea（内部会取消正在进行的 LLM 任务）
    clear_result_textareas();

    // ========== 清理 identify 模式的缓存数据 ==========
    // 1. 清空检测结果上下文（释放字符串内存）
    m_detection_context.clear();
    m_detection_context.shrink_to_fit();

    // 2. 重置聊天模式并恢复 LLM 模式
    if (m_chat_mode) {
        m_chat_mode = false;
        lv_obj_add_flag(m_chat_container, LV_OBJ_FLAG_HIDDEN);
        uint16_t sel = lv_dropdown_get_selected(m_llm_dropdown);
        auto llm = who::llm::LLMClient::get_instance();
        if (llm) llm->set_mode(sel == 1 ? who::llm::LLMMode::VISION : who::llm::LLMMode::LANGUAGE);
    }

    // 4. 清空聊天输入框
    if (m_chat_input) {
        lv_textarea_set_text(m_chat_input, "");
    }

    // ========== 缓存清理结束 ==========

    // 恢复摄像头和检测流水线
    if (m_camera_resume_cb) {
        m_camera_resume_cb();
    }

    bsp_display_lock(0);

    // 恢复表头行显示
    if (m_table_header) lv_obj_clear_flag(m_table_header, LV_OBJ_FLAG_HIDDEN);

    // 恢复Capture按钮
    lv_obj_clear_state(m_capture_btn, LV_STATE_DISABLED);

    // 确保隐藏图片canvas（即使之前因卡顿未创建或显示也处理）
    if (m_image_canvas) {
        lv_obj_add_flag(m_image_canvas, LV_OBJ_FLAG_HIDDEN);
    }

    // 隐藏图片计数标签和删除按钮的父容器
    lv_obj_t *count_delete_container = lv_obj_get_parent(m_image_count_label);
    if (count_delete_container) {
        lv_obj_add_flag(count_delete_container, LV_OBJ_FLAG_HIDDEN);
    }

    // 隐藏 AiChat 按钮（返回摄像头模式时）
    lv_obj_add_flag(m_ai_chat_btn, LV_OBJ_FLAG_HIDDEN);

    // 隐藏键盘和候选面板（如果正在显示）
    if (m_keyboard) {
        lv_obj_add_flag(m_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
    if (m_ime_pinyin) {
        lv_obj_t *panel = lv_ime_pinyin_get_cand_panel(m_ime_pinyin);
        if (panel) lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
    }

    // 显示摄像头容器内除m_image_canvas外的canvas
    lv_obj_t *child0 = lv_obj_get_child(m_camera_container, 0);
    lv_obj_t *child1 = lv_obj_get_child(m_camera_container, 1);

    if (child0 && child0 != m_image_canvas) {
        // 第一个子对象是摄像头canvas，显示它
        lv_obj_clear_flag(child0, LV_OBJ_FLAG_HIDDEN);
    } else if (child1 && child1 != m_image_canvas) {
        // 第二个子对象是摄像头canvas，显示它
        lv_obj_clear_flag(child1, LV_OBJ_FLAG_HIDDEN);
    }

    // 恢复实时检测结果显示
    clear_result_textareas();
    // 摄像头模式使用系统字体
    lv_obj_set_style_text_font(m_result_title_label, &lv_font_montserrat_16, 0);
    lv_label_set_text(m_result_title_label, "Detecting...");

    bsp_display_unlock();
}

void MyAppUI::create_image_canvas()
{
    // 创建图片canvas
    m_image_canvas = lv_canvas_create(m_camera_container);
    lv_obj_set_size(m_image_canvas, IMAGE_DISP_W, IMAGE_DISP_H);
    lv_obj_center(m_image_canvas);
    lv_obj_clear_flag(m_image_canvas, LV_OBJ_FLAG_SCROLLABLE);  // 禁用鼠标滚轮

    // 分配图片缓冲区（RGB565格式，每像素2字节）- 使用SPIRAM
    m_image_buffer = (uint8_t *)heap_caps_malloc(IMAGE_DISP_W * IMAGE_DISP_H * 2, MALLOC_CAP_SPIRAM);
    if (!m_image_buffer) {
        ESP_LOGE(TAG, "Failed to allocate image buffer (SPIRAM)");
        return;
    }

    // 预填充白色背景 - 使用 memset 替代循环（RGB565 0xFFFF = 每字节 0xFF）
    if (m_image_buffer) {
        memset(m_image_buffer, 0xFF, IMAGE_DISP_W * IMAGE_DISP_H * sizeof(uint16_t));
    }

    // 初始状态隐藏
    lv_obj_add_flag(m_image_canvas, LV_OBJ_FLAG_HIDDEN);
}

void MyAppUI::clear_result_textareas()
{
    // 取消正在进行的 LLM 任务（必须先取消，再删除对象，防止 worker 线程访问悬空指针）
    m_llm_task_cancelled = true;

    // 删除实时检测历史记录标签
    for (auto label : m_detect_history) {
        lv_obj_delete(label);
    }
    m_detect_history.clear();
    m_detect_seq = 0;

    // 删除 m_result_textareas 中记录的 textarea
    for (auto ta : m_result_textareas) {
        lv_obj_delete(ta);
    }
    m_result_textareas.clear();

    // 清理 m_result_container 中可能残留的 textarea（如 Vision 模式动态创建的）
    if (m_result_container) {
        uint32_t child_cnt = lv_obj_get_child_count(m_result_container);
        // 从后往前遍历，避免删除后索引变化
        for (int32_t i = child_cnt - 1; i >= 0; i--) {
            lv_obj_t *child = lv_obj_get_child(m_result_container, i);
            if (child == m_result_title_label) continue;
            // 检查是否是 textarea 类型（通过类名判断）
            if (child && lv_obj_check_type(child, &lv_textarea_class)) {
                lv_obj_delete(child);
            }
        }
    }
}

void MyAppUI::update_image_count_label()
{
    if (!m_image_manager || !m_image_count_label) {
        return;
    }

    int current = m_image_manager->get_current_index() + 1;  // 显示从1开始
    int total = m_image_manager->get_image_count();

    static char buf[16];
    snprintf(buf, sizeof(buf), "%d/%d", current, total);

    // 获取容器内的标签对象
    lv_obj_t *count_text = lv_obj_get_child(m_image_count_label, 0);
    if (count_text) {
        bsp_display_lock(0);
        lv_label_set_text(count_text, buf);
        bsp_display_unlock();
    }
}

void MyAppUI::update_image_display()
{
    if (!m_image_manager || !m_image_canvas || !m_image_buffer) {
        return;
    }

    std::string current_path = m_image_manager->get_current_image_path();
    if (current_path.empty()) {
        // 没有图片时，显示背景色
        bsp_display_lock(0);
        // 直接填充白色背景
        if (m_image_buffer) {
            memset(m_image_buffer, 0xFF, IMAGE_DISP_W * IMAGE_DISP_H * sizeof(uint16_t));
        }
        lv_canvas_set_buffer(m_image_canvas, m_image_buffer, IMAGE_DISP_W, IMAGE_DISP_H, LV_COLOR_FORMAT_NATIVE);
        lv_label_set_text(m_result_title_label, "请点击 Capture 拍摄图片");
        bsp_display_unlock();
        return;
    }

    // 读取JPEG文件
    dl::image::jpeg_img_t jpeg_img = dl::image::read_jpeg(current_path.c_str());
    if (!jpeg_img.data) {
        ESP_LOGE(TAG, "Failed to read JPEG: %s", current_path.c_str());
        lv_label_set_text(m_result_title_label, "Failed to load image");
        return;
    }

    // 解码为RGB565LE格式（与LCD显示格式一致）
    dl::image::img_t img = dl::image::sw_decode_jpeg(jpeg_img, dl::image::DL_IMAGE_PIX_TYPE_RGB565LE);
    heap_caps_free(jpeg_img.data);

    if (!img.data) {
        ESP_LOGE(TAG, "Failed to decode JPEG");
        lv_label_set_text(m_result_title_label, "Failed to decode image");
        return;
    }

    // 复制图片数据到缓冲区（RGB565每像素2字节）
    int copy_w = std::min(img.width, IMAGE_DISP_W);
    int copy_h = std::min(img.height, IMAGE_DISP_H);

    // 设置白色背景
    if (m_image_buffer) {
        memset(m_image_buffer, 0xFF, IMAGE_DISP_W * IMAGE_DISP_H * sizeof(uint16_t));
    }

    // 复制图像数据（居中）
    int offset_x = (IMAGE_DISP_W - copy_w) / 2;
    int offset_y = (IMAGE_DISP_H - copy_h) / 2;

    for (int y = 0; y < copy_h; y++) {
        uint8_t *src = (uint8_t *)img.data + y * img.width * 2;
        uint8_t *dst = m_image_buffer + (y + offset_y) * IMAGE_DISP_W * 2 + offset_x * 2;
        memcpy(dst, src, copy_w * 2);
    }

    // 设置canvas缓冲区（使用NATIVE格式，自动匹配LCD配置的RGB565）
    lv_canvas_set_buffer(m_image_canvas, m_image_buffer, IMAGE_DISP_W, IMAGE_DISP_H, LV_COLOR_FORMAT_NATIVE);

    // 释放解码的图片数据
    heap_caps_free(img.data);

    // 更新结果显示标题
    bsp_display_lock(0);
    lv_label_set_text(m_result_title_label, "点击启动识别按钮开始科普");
    bsp_display_unlock();
}


void MyAppUI::perform_single_image_detect()
{
    if (!m_image_manager || !m_detect_model || !m_image_buffer) {
        return;
    }

    std::string current_path = m_image_manager->get_current_image_path();

    // 读取JPEG文件
    dl::image::jpeg_img_t jpeg_img = dl::image::read_jpeg(current_path.c_str());
    if (!jpeg_img.data) {
        ESP_LOGE(TAG, "Failed to read JPEG for detection");
        bsp_display_lock(0);
        lv_label_set_text(m_result_title_label, "Failed to load image!");
        bsp_display_unlock();
        return;
    }

    // 解码为RGB565LE格式（与LCD显示格式一致）
    dl::image::img_t img = dl::image::sw_decode_jpeg(jpeg_img, dl::image::DL_IMAGE_PIX_TYPE_RGB565LE);

    if (!img.data) {
        ESP_LOGE(TAG, "Failed to decode JPEG");
        heap_caps_free(jpeg_img.data);
        bsp_display_lock(0);
        lv_label_set_text(m_result_title_label, "Failed to decode!");
        bsp_display_unlock();
        return;
    }

    // 执行检测
    int64_t t_yolo_start = esp_timer_get_time();
    auto results = m_detect_model->run(img);
    int64_t t_yolo_end = esp_timer_get_time();
    update_infer_time((t_yolo_end - t_yolo_start) / 1000);
    if (!results.empty()) {
        int i = 0;
        for (const auto &r : results) {
            ESP_LOGI(TAG, "[PERF] YOLO推理: %lld ms, 检出%d个, [%d] 类别=%d(%s) 置信度=%.2f 框=(%d,%d,%d,%d) 图片%dx%d",
                     (t_yolo_end - t_yolo_start) / 1000, results.size(), i,
                     r.category, coco_classes[r.category], r.score,
                     r.box[0], r.box[1], r.box[2], r.box[3],
                     img.width, img.height);
            i++;
        }
    }

    // 获取当前LLM模式
    auto llm_client = who::llm::LLMClient::get_instance();
    bool is_vision_mode = llm_client && llm_client->get_mode() == who::llm::LLMMode::VISION;

    // Vision模式：重新编码为低质量JPEG（防止SDIO卡顿）
    if (is_vision_mode) {
        int64_t t0 = esp_timer_get_time();
        dl::image::jpeg_img_t low_q_jpeg = dl::image::sw_encode_jpeg(img, 20);
        int64_t t1 = esp_timer_get_time();
        if (low_q_jpeg.data) {
            heap_caps_free(jpeg_img.data);
            jpeg_img = low_q_jpeg;
        } else {
            ESP_LOGW(TAG, "Failed to re-encode JPEG, using original (SDIO crash risk)");
        }
    }
    // 在图片上绘制检测框（RGB565LE格式的颜色值）
    std::vector<std::vector<uint8_t>> palette = {{0x00, 0xF8}};  // RGB565LE红色检测框
    if (!results.empty()) {
        detect::SingleImageDetect::draw_results_on_img(img, results, palette);
    }

    // 直接将结果图片显示在canvas上（RGB565LE格式）
    int offset_x = 0, offset_y = 0;
    {
        int copy_w = std::min(img.width, IMAGE_DISP_W);
        int copy_h = std::min(img.height, IMAGE_DISP_H);
        // 设置白色背景
        if (m_image_buffer) {
            memset(m_image_buffer, 0xFF, IMAGE_DISP_W * IMAGE_DISP_H * sizeof(uint16_t));
        }

        offset_x = (IMAGE_DISP_W - copy_w) / 2;
        offset_y = (IMAGE_DISP_H - copy_h) / 2;

        for (int y = 0; y < copy_h; y++) {
            uint8_t *src = (uint8_t *)img.data + y * img.width * 2;
            uint8_t *dst = m_image_buffer + (y + offset_y) * IMAGE_DISP_W * 2 + offset_x * 2;
            memcpy(dst, src, copy_w * 2);
        }

        bsp_display_lock(0);
        lv_canvas_set_buffer(m_image_canvas, m_image_buffer, IMAGE_DISP_W, IMAGE_DISP_H, LV_COLOR_FORMAT_NATIVE);

        // 在canvas上绘制物种名称标签
        if (!results.empty()) {
            lv_draw_label_dsc_t label_dsc;
            lv_draw_label_dsc_init(&label_dsc);
            label_dsc.color = lv_color_white();
            label_dsc.align = LV_TEXT_ALIGN_LEFT;
            label_dsc.font = &lv_font_alibabapuhuiti_light_16;

            lv_draw_rect_dsc_t bg_dsc;
            lv_draw_rect_dsc_init(&bg_dsc);
            bg_dsc.bg_opa = LV_OPA_70;
            bg_dsc.border_width = 0;
            bg_dsc.radius = 2;
            bg_dsc.bg_color = lv_color_hex(0xF44336);  // 红色背景

            lv_layer_t layer;
            lv_canvas_init_layer(m_image_canvas, &layer);

            for (const auto &res : results) {
                const char *name = coco_classes[res.category];
                if (!name) continue;

                lv_point_t txt_size;
                lv_text_get_size(&txt_size, name, &lv_font_alibabapuhuiti_light_16, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
                int32_t lw = txt_size.x + 8;
                int32_t lh = txt_size.y + 4;
                int32_t lx = res.box[0] + offset_x;
                int32_t ly = res.box[1] + offset_y - lh - 2;
                if (ly < 0) ly = 0;

                // 半透明背景
                lv_area_t bg_area = {lx, ly, lx + lw - 1, ly + lh - 1};
                lv_draw_rect(&layer, &bg_dsc, &bg_area);

                // 文字
                lv_area_t label_area = {lx + 4, ly + 2, lx + lw - 5, ly + lh - 3};
                label_dsc.text = name;
                lv_draw_label(&layer, &label_dsc, &label_area);
            }

            lv_canvas_finish_layer(m_image_canvas, &layer);
        }

        bsp_display_unlock();
    }

    // 释放RGB565LE内存（不保存结果图片）
    heap_caps_free(img.data);
    img.data = nullptr;

    bsp_display_lock(0);

    // identify 模式使用中文字体
    lv_obj_set_style_text_font(m_result_title_label, &lv_font_alibabapuhuiti_light_16, 0);
    lv_label_set_text(m_result_title_label, "识别结果");

    // 更新标题
    if (results.empty()) {
        if (is_vision_mode && jpeg_img.data && jpeg_img.data_len > 0) {
            lv_label_set_text(m_result_title_label, "识别结果：通用识别中...");
            if (m_result_textareas.empty()) {
                lv_obj_t* ta = lv_textarea_create(m_result_container);
                lv_textarea_set_text(ta, "通用识别中...");
                lv_textarea_set_one_line(ta, false);
                lv_textarea_set_max_length(ta, 0);
                lv_obj_remove_style(ta, nullptr, LV_PART_SCROLLBAR);
                lv_obj_add_flag(ta, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
                lv_obj_set_scrollbar_mode(ta, LV_SCROLLBAR_MODE_OFF);
                lv_obj_set_size(ta, LV_PCT(100), LV_SIZE_CONTENT);
                lv_obj_set_style_text_align(ta, LV_TEXT_ALIGN_LEFT, 0);
                lv_obj_set_style_bg_color(ta, lv_color_hex(0x4A6B8A), 0);
                lv_obj_set_style_text_color(ta, lv_color_hex(0xffffff), 0);
                lv_obj_set_style_text_font(ta, &lv_font_alibabapuhuiti_light_16, 0);
                lv_obj_set_style_border_width(ta, 1, 0);
                lv_obj_set_style_border_color(ta, lv_color_hex(0x008FB2), 0);
                lv_obj_set_style_radius(ta, 6, 0);
                lv_obj_set_style_pad_all(ta, 8, 0);
                m_result_textareas.push_back(ta);
            } else {
                // 清空现有 textarea
                for (size_t i = 0; i < m_result_textareas.size(); i++) {
                    lv_textarea_set_text(m_result_textareas[i], "通用识别中...");
                }
            }

            // 将JPEG图片编码为base64
            std::string image_base64 = who::utils::base64_encode((const uint8_t*)jpeg_img.data, jpeg_img.data_len);
            ESP_LOGI(TAG, "Empty YOLO results, encoding image for general recognition, base64 length: %d", image_base64.length());

            // 释放JPEG数据
            heap_caps_free(jpeg_img.data);
            jpeg_img.data = nullptr;

            // 保存任务参数（空物种列表）
            struct VisionTaskParams {
                std::string image_base64;
                std::vector<std::string> yolo_names;
                std::vector<lv_obj_t*> textareas;
                bool* cancelled_flag;
                std::string* detection_context;
                MyAppUI* app;
            };
            VisionTaskParams* params = new VisionTaskParams{image_base64, {}, m_result_textareas, &m_llm_task_cancelled, &m_detection_context, this};

            // 释放锁，让task可以获取锁来更新UI
            bsp_display_unlock();

            // 投递到 LLM 工作线程
            llm_worker_init();
            LLMTaskMsg msg = {[](void* arg) {
                VisionTaskParams* params = static_cast<VisionTaskParams*>(arg);

                auto llm = who::llm::LLMClient::get_instance();
                llm->init();

                // 发送视觉识别请求（空物种列表，会使用通用识别提示词）
                int64_t t_llm_start = esp_timer_get_time();
                std::string vision_result = llm->send_vision_request(params->image_base64.c_str(), params->yolo_names);
                int64_t t_llm_end = esp_timer_get_time();
                params->app->update_infer_time((t_llm_end - t_llm_start) / 1000);

                // 检查是否已取消
                if (params->cancelled_flag && *(params->cancelled_flag)) {
                    delete params;
                    return;
                }

                bsp_display_lock(0);
                if (params->cancelled_flag && *(params->cancelled_flag)) {
                    bsp_display_unlock();
                    delete params;
                    return;
                }

                if (vision_result.empty()) {
                    ESP_LOGE(TAG, "General recognition failed");
                    for (size_t i = 0; i < params->textareas.size(); i++) {
                        lv_textarea_set_text(params->textareas[i], "识别失败: 未检测到物体");
                    }
                } else {
                    ESP_LOGI(TAG, "General recognition result: %s", vision_result.c_str());

                    auto species_list = parse_vision_species_response(vision_result);

                    if (!species_list.empty()) {
                        // 格式化显示
                        std::string display_text;
                        for (size_t i = 0; i < species_list.size(); i++) {
                            if (i > 0) display_text += "\n\n";
                            display_text += species_list[i].first + "\n" + species_list[i].second;
                        }
                        if (!params->textareas.empty()) {
                            lv_textarea_set_text(params->textareas[0], display_text.c_str());
                        }
                        // 格式化检测上下文
                        if (params->detection_context) {
                            std::string ctx;
                            for (const auto& sp : species_list) {
                                ctx += "【" + sp.first + "】\n" + sp.second + "\n\n";
                            }
                            *params->detection_context = ctx;
                        }
                    } else {
                        // JSON 和旧格式都解析失败，直接显示原始文本
                        if (!params->textareas.empty()) {
                            lv_textarea_set_text(params->textareas[0], vision_result.c_str());
                        }
                        if (params->detection_context) {
                            *params->detection_context = vision_result;
                        }
                    }
                }

                // 更新标题
                lv_label_set_text(params->app->m_result_title_label, "识别结果");

                // Vision 模式下识别完成后显示 AiChat 按钮（如果有 WiFi）
                auto wifi_mgr = who::wifi::WifiManager::get_instance();
                if (wifi_mgr && wifi_mgr->is_connected()) {
                    lv_obj_clear_flag(params->app->m_ai_chat_btn, LV_OBJ_FLAG_HIDDEN);
                    lv_obj_align(params->app->m_ai_chat_btn, LV_ALIGN_TOP_MID, 0, 7);
                }

                bsp_display_unlock();

                // AI 回答已到达，通知用户
                if (params->app->m_voice_enabled) who::sys::tts_speak("识别完成");

                delete params;
            }, params};
            xQueueSend(s_llm_queue, &msg, portMAX_DELAY);

            return;
        }

        // 非 Vision 模式：直接结束
        lv_label_set_text(m_result_title_label, "识别结果：未检测到物体");
        if (m_voice_enabled) who::sys::tts_speak("识别失败");
        // 释放JPEG数据
        if (jpeg_img.data) {
            heap_caps_free(jpeg_img.data);
        }
        // 如果处于聊天模式，退出聊天模式
        if (m_chat_mode) {
            m_chat_mode = false;
            lv_obj_add_flag(m_chat_container, LV_OBJ_FLAG_HIDDEN);
            // 恢复 identify 模式下的按钮
            lv_obj_clear_flag(m_browse_container, LV_OBJ_FLAG_HIDDEN);
            lv_obj_t *count_delete_container = lv_obj_get_parent(m_image_count_label);
            if (count_delete_container) {
                lv_obj_clear_flag(count_delete_container, LV_OBJ_FLAG_HIDDEN);
            }
            // 按钮标签改回 AiChat
            lv_obj_t *label = lv_obj_get_child(m_ai_chat_btn, 0);
            if (label) {
                lv_label_set_text(label, "Ai聊天");
            }
            // 结果容器高度恢复为420
            lv_obj_set_size(m_result_container, 400, 420);
        }
        // 隐藏 AiChat 按钮
        lv_obj_add_flag(m_ai_chat_btn, LV_OBJ_FLAG_HIDDEN);
        bsp_display_unlock();
        return;
    }

    lv_label_set_text(m_result_title_label, "识别结果");
    // TTS "识别完成" 移到异步 LLM 任务回调内部，AI 回答到达后再播放

    // 检查 WiFi 是否已连接
    auto wifi_mgr = who::wifi::WifiManager::get_instance();
    bool wifi_connected = wifi_mgr && wifi_mgr->is_connected();

    // 只有 WiFi 连接时才显示 AiChat 按钮
    if (wifi_connected) {
        lv_obj_clear_flag(m_ai_chat_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(m_ai_chat_btn, LV_ALIGN_TOP_MID, 0, 7);
    } else {
        lv_obj_add_flag(m_ai_chat_btn, LV_OBJ_FLAG_HIDDEN);
        // WiFi未连接时释放JPEG数据
        if (jpeg_img.data) {
            heap_caps_free(jpeg_img.data);
            jpeg_img.data = nullptr;
        }
    }

    bsp_display_unlock();

    // Language 模式下无需保留 JPEG 数据（仅 Vision 模式后续需要 base64 编码图片）
    if (!is_vision_mode && jpeg_img.data) {
        heap_caps_free(jpeg_img.data);
        jpeg_img.data = nullptr;
    }

    // 合并相同物种的检测结果
    std::map<int, std::pair<int, float>> species_map;
    for (const auto &res : results) {
        auto it = species_map.find(res.category);
        if (it == species_map.end()) {
            species_map[res.category] = {1, res.score};
        } else {
            it->second.first++;
            if (res.score > it->second.second) {
                it->second.second = res.score;
            }
        }
    }

    // 构建检测结果上下文（用于 AI Chat）
    m_detection_context.clear();
    for (const auto &kv : species_map) {
        int category = kv.first;
        int count = kv.second.first;
        float max_score = kv.second.second;
        char buf[128];
        if (count > 1) {
            snprintf(buf, sizeof(buf), "- %s x%d (置信度: %.0f%%)\n",
                     coco_classes[category], count, max_score * 100);
        } else {
            snprintf(buf, sizeof(buf), "- %s (置信度: %.0f%%)\n",
                     coco_classes[category], max_score * 100);
        }
        m_detection_context += buf;
    }

    // 为每个不同物种创建一个 textarea
    int idx = 1;
    for (const auto &kv : species_map) {
        int category = kv.first;
        int count = kv.second.first;
        float max_score = kv.second.second;

        // 创建多行文本框
        lv_obj_t *ta = lv_textarea_create(m_result_container);
        lv_obj_set_size(ta, 370, LV_SIZE_CONTENT);  // 宽度固定，高度自适应
        lv_textarea_set_one_line(ta, false);  // 多行模式
        lv_textarea_set_max_length(ta, 0);     // 不限字符长度

        // 禁用滚动条
        lv_obj_remove_style(ta, nullptr, LV_PART_SCROLLBAR);
        lv_obj_add_flag(ta, LV_OBJ_FLAG_SCROLL_ON_FOCUS);  // 禁用滚动
        lv_obj_set_scrollbar_mode(ta, LV_SCROLLBAR_MODE_OFF);  // 隐藏滚动条

        // 启用自动换行
        lv_obj_set_style_text_align(ta, LV_TEXT_ALIGN_LEFT, 0);

        // 设置样式
        lv_obj_set_style_bg_color(ta, lv_color_hex(0x4A6B8A), 0);
        lv_obj_set_style_text_color(ta, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(ta, &lv_font_alibabapuhuiti_light_16, 0);
        lv_obj_set_style_border_width(ta, 1, 0);
        lv_obj_set_style_border_color(ta, lv_color_hex(0x008FB2), 0);
        lv_obj_set_style_radius(ta, 6, 0);
        lv_obj_set_style_pad_all(ta, 8, 0);
        lv_obj_set_style_min_height(ta, 60, 0);

        // 第一行：物种名称、数量和最高置信度
        char first_line[128];
        if (count > 1) {
            snprintf(first_line, sizeof(first_line), "物种%d：%s x%-35d   置信度:%.0f%%\n",
                     idx, coco_classes[category], count, max_score * 100);
        } else {
            snprintf(first_line, sizeof(first_line), "物种%d：%-40s   置信度:%.0f%%\n",
                     idx, coco_classes[category], max_score * 100);
        }

        // 设置初始文本
        std::string initial_text;
        if (wifi_connected) {
            initial_text = std::string(first_line) + "\n加载中...";
        } else {
            initial_text = std::string(first_line);
        }
        lv_textarea_set_text(ta, initial_text.c_str());

        // 为 textarea 添加点击事件：点击时隐藏键盘和候选面板
        lv_obj_add_event_cb(ta, [](lv_event_t *e) {
            MyAppUI *app = (MyAppUI*)lv_event_get_user_data(e);
            if (!app || !app->m_keyboard) return;
            lv_obj_add_flag(app->m_keyboard, LV_OBJ_FLAG_HIDDEN);
            if (app->m_ime_pinyin) {
                lv_obj_t *panel = lv_ime_pinyin_get_cand_panel(app->m_ime_pinyin);
                if (panel) lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
            }
        }, LV_EVENT_CLICKED, this);

        m_result_textareas.push_back(ta);
        idx++;
    }

    // 如果 WiFi 已连接，根据LLM模式启动不同的识别任务
    if (wifi_connected && !species_map.empty()) {
        // 收集所有物种名称
        std::vector<std::string> species_names;
        std::vector<std::pair<int, int>> species_info;  // {category, count}
        for (const auto &kv : species_map) {
            species_names.push_back(coco_classes[kv.first]);
            species_info.push_back({kv.first, kv.second.first});
        }

        if (is_vision_mode) {
            // ========== Vision模式：发送图片和YOLO结果到云端AI进行精细化识别 ==========
            int64_t t_be = esp_timer_get_time();
            // 将JPEG图片编码为base64
            std::string image_base64 = who::utils::base64_encode((const uint8_t*)jpeg_img.data, jpeg_img.data_len);
            int64_t t_af = esp_timer_get_time();

            // 释放JPEG数据（Vision模式下不再需要）
            heap_caps_free(jpeg_img.data);
            jpeg_img.data = nullptr;

            // 保存任务参数
            struct VisionTaskParams {
                std::string image_base64;
                std::vector<std::string> yolo_names;          // YOLO识别的物种名
                std::vector<float> yolo_scores;               // YOLO置信度
                std::vector<lv_obj_t*> textareas;
                bool* cancelled_flag;
                std::string* detection_context;
                MyAppUI* app;
            };

            // 收集每个物种的 YOLO 最高置信度（与 species_map 顺序一致）
            std::vector<float> yolo_max_scores;
            for (const auto &kv : species_map) {
                yolo_max_scores.push_back(kv.second.second);
            }

            VisionTaskParams* params = new VisionTaskParams{image_base64, species_names, yolo_max_scores, m_result_textareas, &m_llm_task_cancelled, &m_detection_context, this};

            // 投递到 LLM 工作线程
            llm_worker_init();
            LLMTaskMsg msg = {[](void* arg) {
                VisionTaskParams* params = static_cast<VisionTaskParams*>(arg);

                // 初始化 LLM 客户端
                auto llm = who::llm::LLMClient::get_instance();
                llm->init();

                // 计时 LLM 视觉识别请求
                int64_t t_llm_start = esp_timer_get_time();
                std::string vision_result = llm->send_vision_request(params->image_base64.c_str(), params->yolo_names);
                int64_t t_llm_end = esp_timer_get_time();
                params->app->update_infer_time((t_llm_end - t_llm_start) / 1000);

                // 检查是否已取消
                if (params->cancelled_flag && *(params->cancelled_flag)) {
                    ESP_LOGI(TAG, "Vision task cancelled, discarding results");
                    delete params;
                    return;
                }

                // 更新 UI
                bsp_display_lock(0);
                if (params->cancelled_flag && *(params->cancelled_flag)) {
                    bsp_display_unlock();
                    ESP_LOGI(TAG, "Vision task cancelled during UI update, discarding results");
                    delete params;
                    return;
                }

                if (vision_result.empty()) {
                    // 识别失败
                    ESP_LOGE(TAG, "Vision recognition failed");
                    for (size_t i = 0; i < params->textareas.size(); i++) {
                        lv_textarea_set_text(params->textareas[i], "识别失败: 未检测到物体");
                    }
                } else {
                    ESP_LOGI(TAG, "Vision result: %s", vision_result.c_str());

                    auto species_list = parse_vision_species_response(vision_result);

                    if (species_list.size() > params->textareas.size()) {
                        size_t extra_count = species_list.size() - params->textareas.size();
                        for (size_t j = 0; j < extra_count; j++) {
                            lv_obj_t *ta = lv_textarea_create(lv_obj_get_parent(params->textareas[0]));
                            lv_obj_set_size(ta, 370, LV_SIZE_CONTENT);
                            lv_textarea_set_one_line(ta, false);
                            lv_textarea_set_max_length(ta, 0);
                            lv_obj_remove_style(ta, nullptr, LV_PART_SCROLLBAR);
                            lv_obj_add_flag(ta, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
                            lv_obj_set_scrollbar_mode(ta, LV_SCROLLBAR_MODE_OFF);
                            lv_obj_set_style_text_align(ta, LV_TEXT_ALIGN_LEFT, 0);
                            lv_obj_set_style_bg_color(ta, lv_color_hex(0x4A6B8A), 0);
                            lv_obj_set_style_text_color(ta, lv_color_hex(0xffffff), 0);
                            lv_obj_set_style_text_font(ta, &lv_font_alibabapuhuiti_light_16, 0);
                            lv_obj_set_style_border_width(ta, 1, 0);
                            lv_obj_set_style_border_color(ta, lv_color_hex(0x008FB2), 0);
                            lv_obj_set_style_radius(ta, 6, 0);
                            lv_obj_set_style_pad_all(ta, 8, 0);
                            lv_obj_set_style_min_height(ta, 60, 0);
                            lv_textarea_set_text(ta, "加载中...");
                            params->textareas.push_back(ta);
                        }
                    }

                    for (size_t i = 0; i < params->textareas.size(); i++) {
                        float cur_score = (i < params->yolo_scores.size()) ? params->yolo_scores[i] : (params->yolo_scores.empty() ? 0 : params->yolo_scores.back());

                        std::string precise_name;
                        std::string detail;
                        if (i < species_list.size()) {
                            precise_name = species_list[i].first;
                            detail = species_list[i].second;
                        } else {
                            precise_name = (i < params->yolo_names.size()) ? params->yolo_names[i] : "未知";
                            detail = "介绍获取失败";
                        }

                        char first_line[128];
                        snprintf(first_line, sizeof(first_line), "物种%zu：%-40s   置信度:%.0f%%\n",
                                 i + 1, precise_name.c_str(), cur_score * 100);

                        std::string full_text = std::string(first_line) + detail;
                        lv_textarea_set_text(params->textareas[i], full_text.c_str());
                    }

                    if (params->detection_context) {
                        // 格式化检测上下文（供 AI Chat 使用）
                        std::string ctx;
                        for (const auto& sp : species_list) {
                            ctx += "【" + sp.first + "】\n" + sp.second + "\n\n";
                        }
                        *(params->detection_context) = ctx;
                    }
                }

                bsp_display_unlock();

                // AI 回答已到达，通知用户
                if (params->app->m_voice_enabled) who::sys::tts_speak("识别完成");

                delete params;
            }, params};
            xQueueSend(s_llm_queue, &msg, portMAX_DELAY);
        } else {
            // ========== Language模式：发送物种名称列表到云端AI,进行批量获取科普介绍 ==========
            // 释放JPEG数据（Language模式不需要）
            heap_caps_free(jpeg_img.data);
            jpeg_img.data = nullptr;

            // 保存 textarea 指针和对应信息
            struct BatchTaskParams {
                std::vector<std::string> species_names;
                std::vector<std::pair<int, int>> species_info;
                std::vector<lv_obj_t*> textareas;
                std::vector<std::string> first_lines;
                bool* cancelled_flag;  // 取消标志指针
                std::string* detection_context;  // 检测结果上下文指针
                MyAppUI* app;
            };
            BatchTaskParams* params = new BatchTaskParams{species_names, species_info, m_result_textareas, {}, &m_llm_task_cancelled, &m_detection_context, this};

            for (size_t i = 0; i < m_result_textareas.size(); i++) {
                std::string full_text = lv_textarea_get_text(m_result_textareas[i]);
                size_t newline_pos = full_text.find('\n');
                if (newline_pos != std::string::npos) {
                    params->first_lines.push_back(full_text.substr(0, newline_pos));
                } else {
                    params->first_lines.push_back(full_text);
                }
            }

            // 投递到 LLM 工作线程
            llm_worker_init();
            LLMTaskMsg msg = {[](void* arg) {
                BatchTaskParams* params = static_cast<BatchTaskParams*>(arg);

                // 初始化 LLM 客户端
                auto llm = who::llm::LLMClient::get_instance();
                llm->init();

                // 批量生成科普介绍（含计时）
                int64_t t_llm_start = esp_timer_get_time();
                auto science_map = llm->generate_science_intro_batch(params->species_names);
                int64_t t_llm_end = esp_timer_get_time();
                params->app->update_infer_time((t_llm_end - t_llm_start) / 1000);

                // 检查是否已取消
                if (params->cancelled_flag && *(params->cancelled_flag)) {
                    ESP_LOGI(TAG, "LLM task cancelled, discarding results");
                    delete params;
                    return;
                }

                // 更新 UI
                bsp_display_lock(0);
                // 再次检查取消标志（防止在获取锁期间被取消）
                if (params->cancelled_flag && *(params->cancelled_flag)) {
                    bsp_display_unlock();
                    ESP_LOGI(TAG, "LLM task cancelled during UI update, discarding results");
                    delete params;
                    return;
                }
                // 构建包含科普内容的检测结果上下文
                std::string full_context;
                for (size_t i = 0; i < params->textareas.size() && i < params->species_names.size(); i++) {
                    const std::string& species = params->species_names[i];
                    std::string intro = science_map.count(species) ? science_map[species] : "";

                    // LLM 返回空或暂无时，尝试从本地知识库获取
                    if (intro.empty() || intro == "暂无科普信息" || intro == "科普内容获取失败") {
                        who::sys::knowledge_init();
                        const char* local = who::sys::knowledge_query(species.c_str());
                        if (local) {
                            intro = local;
                        } else if (intro.empty()) {
                            intro = "暂无科普信息";
                        }
                    }

                    // 组合第一行和科普内容
                    std::string full_text = params->first_lines[i] + "\n" + intro;
                    lv_textarea_set_text(params->textareas[i], full_text.c_str());

                    // 构建上下文（包含物种信息和科普内容）
                    full_context += "【" + species + "】\n";
                    full_context += intro + "\n\n";
                }

                // 保存到检测结果上下文
                if (params->detection_context) {
                    *(params->detection_context) = full_context;
                }

                bsp_display_unlock();

                // AI 回答已到达，通知用户
                if (params->app->m_voice_enabled) who::sys::tts_speak("识别完成");

                delete params;
            }, params};
            xQueueSend(s_llm_queue, &msg, portMAX_DELAY);
        }
    }

    // WiFi 未连接但检测到物体时，使用本地知识库
    if (!wifi_connected && !species_map.empty()) {

        // 初始化本地知识库
        who::sys::knowledge_init();

        bsp_display_lock(0);
        int idx = 0;
        for (const auto &kv : species_map) {
            if (idx >= (int)m_result_textareas.size()) break;

            const char *species_name = coco_classes[kv.first];
            const char *intro = who::sys::knowledge_query(species_name);

            if (intro) {
                std::string full_text = lv_textarea_get_text(m_result_textareas[idx]);
                full_text += "\n";
                full_text += intro;
                lv_textarea_set_text(m_result_textareas[idx], full_text.c_str());
            }
            idx++;
        }
        bsp_display_unlock();

        if (m_voice_enabled) who::sys::tts_speak("识别完成");
    }
}



// 供外部检测回调调用：将检测结果喂入 Assist 逻辑
void MyAppUI::feed_detect_result(const std::list<dl::detect::result_t> &det_res)
{
    if (!m_assist_enabled) return;
    if (m_identify_mode) {
        ESP_LOGD(TAG, "Assist: skip, identify mode");
        return;
    }
    if (m_assist_capture_pending) {
        ESP_LOGD(TAG, "Assist: skip, capture pending");
        return;
    }
    assist_on_detect_result(det_res);
}

// Assist 核心逻辑：跟踪5秒内检测频率，满足阈值则自动触发capture
void MyAppUI::assist_on_detect_result(const std::list<dl::detect::result_t> &det_res)
{
    if (det_res.empty()) return;

    int64_t now_ms = esp_timer_get_time() / 1000;  // 微秒转毫秒

    // 记录本次检测到的每个物体类别
    for (const auto &r : det_res) {
        m_assist_records.push_back({r.category, now_ms});
    }

    // 清除超过5秒窗口的旧记录
    int64_t cutoff = now_ms - ASSIST_WINDOW_MS;
    size_t removed = 0;
    while (!m_assist_records.empty() && m_assist_records.front().timestamp_ms < cutoff) {
        m_assist_records.erase(m_assist_records.begin());
        removed++;
    }

    // 统计5秒内每个类别的检测次数
    std::map<int, int> category_counts;
    for (const auto &rec : m_assist_records) {
        category_counts[rec.category]++;
    }

    // 找到检测次数最多的类别
    int max_count = 0;
    int max_category = -1;
    for (const auto &kv : category_counts) {
        if (kv.second > max_count) {
            max_count = kv.second;
            max_category = kv.first;
        }
    }

    // 输出当前窗口统计
    ESP_LOGI(TAG, "Assist: window=%zu records (removed %zu), %zu categories, top=cat%d count=%d/%d",
             m_assist_records.size(), removed, category_counts.size(),
             max_category, max_count, ASSIST_MIN_COUNT);

    // 判断是否满足触发条件
    if (max_count >= ASSIST_MIN_COUNT) {
        ESP_LOGI(TAG, "Assist: category %d detected %d times in 5s (threshold=%d), triggering auto-capture",
                 max_category, max_count, ASSIST_MIN_COUNT);
        m_assist_records.clear();        // 清空记录避免重复触发
        m_assist_capture_pending = true;  // 标记正在等待capture完成
        assist_trigger_capture();
    }
}

// 自动触发capture（拍照+识别）
void MyAppUI::assist_trigger_capture()
{
    ESP_LOGI(TAG, "Assist: creating auto-capture task");
    struct CaptureParams { MyAppUI* app; };
    CaptureParams* params = new CaptureParams{this};

    TaskHandle_t task_handle = nullptr;
    BaseType_t ret = xTaskCreatePinnedToCoreWithCaps([](void* arg) {
        CaptureParams* p = static_cast<CaptureParams*>(arg);

        ESP_LOGI(TAG, "Assist: auto-capture -> CMD_PAI_ZHAO");
        p->app->execute_voice_command(speech::VoiceCommand::CMD_PAI_ZHAO);
        vTaskDelay(pdMS_TO_TICKS(500));

        ESP_LOGI(TAG, "Assist: auto-capture -> CMD_SHI_BIE");
        p->app->execute_voice_command(speech::VoiceCommand::CMD_SHI_BIE);

        // capture流程完成后，重置标志
        vTaskDelay(pdMS_TO_TICKS(1000));
        p->app->m_assist_capture_pending = false;
        ESP_LOGI(TAG, "Assist: auto-capture done, pending flag cleared");

        delete p;
        vTaskDelete(nullptr);
    }, "assist_capture", 8 * 1024, params, 5, &task_handle, 0, MALLOC_CAP_SPIRAM);

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Assist: FAILED to create auto-capture task! ret=%d", ret);
        m_assist_capture_pending = false;
        delete params;
    }
}


// 更新 WiFi 按钮状态显示
void MyAppUI::update_wifi_button_state(who::wifi::WifiState state)
{
    if (!m_wifi_btn) return;

    bsp_display_lock(0);

    switch (state) {
        case who::wifi::WifiState::CONNECTED:
            // 连接成功 - 绿色背景
            lv_obj_set_style_bg_color(m_wifi_btn, lv_color_hex(0x4CAF50), 0);
            if (m_voice_enabled) who::sys::tts_speak("网络连接成功");
            break;
        case who::wifi::WifiState::DISCONNECTED:
            // 断开 - 红色背景
            lv_obj_set_style_bg_color(m_wifi_btn, lv_color_hex(0xF44336), 0);
            if (m_voice_enabled) who::sys::tts_speak("网络已断开");
            break;
        case who::wifi::WifiState::CONNECTING:
            // 连接中 - 红色背景
            lv_obj_set_style_bg_color(m_wifi_btn, lv_color_hex(0xF44336), 0);
            if (m_voice_enabled) who::sys::tts_speak("正在连接网络");
            break;
        case who::wifi::WifiState::FAILED:
            // 连接失败 - 红色背景
            lv_obj_set_style_bg_color(m_wifi_btn, lv_color_hex(0xF44336), 0);
            if (m_voice_enabled) who::sys::tts_speak("网络连接失败");
            break;
        default:
            // 其他状态 - 红色背景
            lv_obj_set_style_bg_color(m_wifi_btn, lv_color_hex(0xF44336), 0);
            break;
    }

    bsp_display_unlock();
}

// AiChat 按钮点击回调 - 切换聊天模式

// 执行语音命令
void MyAppUI::execute_voice_command(speech::VoiceCommand cmd)
{
    switch (cmd) {
        case speech::VoiceCommand::CMD_PAI_ZHAO: {
            // 帮我拍照
            if (!m_identify_mode) {
                if (!m_frame_cap_node) {
                    ESP_LOGE(TAG, "Frame capture node not set");
                    break;
                }
                if (!m_image_manager) {
                    ESP_LOGE(TAG, "Image manager not initialized");
                    break;
                }
                who::cam::cam_fb_t *fb = m_frame_cap_node->cam_fb_peek();
                if (!fb || !fb->buf) {
                    ESP_LOGE(TAG, "Failed to get current frame");
                    break;
                }
                std::string saved_path = m_image_manager->save_capture_frame(fb);
                if (!saved_path.empty()) {
                    ESP_LOGI(TAG, "Capture saved: %s", saved_path.c_str());
                } else {
                    ESP_LOGE(TAG, "Failed to save frame");
                }
            } else {
                ESP_LOGW(TAG, "Cannot capture in identify mode");
            }
            break;
        }

        case speech::VoiceCommand::CMD_SHI_BIE: {
            // 帮我识别
            if (m_identify_mode) {
                bsp_display_lock(0);
                if (m_start_btn) {
                    lv_obj_send_event(m_start_btn, LV_EVENT_CLICKED, this);
                }
                bsp_display_unlock();
            } else {
                bsp_display_lock(0);
                if (m_identify_btn) {
                    lv_obj_send_event(m_identify_btn, LV_EVENT_CLICKED, this);
                }
                bsp_display_unlock();
                vTaskDelay(pdMS_TO_TICKS(500));
                bsp_display_lock(0);
                if (m_start_btn) {
                    lv_obj_send_event(m_start_btn, LV_EVENT_CLICKED, this);
                }
                bsp_display_unlock();
            }
            break;
        }

        case speech::VoiceCommand::CMD_SHAN_CHU: {
            // 帮我删除照片 - 只有在识别模式下才能删除
            bsp_display_lock(0);
            if (m_identify_mode && m_delete_btn) {
                lv_obj_send_event(m_delete_btn, LV_EVENT_CLICKED, this);
            } else {
                ESP_LOGW(TAG, "Cannot delete in camera mode");
            }
            bsp_display_unlock();
            break;
        }

        case speech::VoiceCommand::CMD_XIA_YI_ZHANG: {
            // 下一张 - 只有在识别模式下才能浏览
            bsp_display_lock(0);
            if (m_identify_mode && m_next_btn) {
                lv_obj_send_event(m_next_btn, LV_EVENT_CLICKED, this);
            } else {
                ESP_LOGW(TAG, "Cannot browse in camera mode");
            }
            bsp_display_unlock();
            break;
        }

        case speech::VoiceCommand::CMD_SHANG_YI_ZHANG: {
            // 上一张 - 只有在识别模式下才能浏览
            bsp_display_lock(0);
            if (m_identify_mode && m_pre_btn) {
                lv_obj_send_event(m_pre_btn, LV_EVENT_CLICKED, this);
            } else {
                ESP_LOGW(TAG, "Cannot browse in camera mode");
            }
            bsp_display_unlock();
            break;
        }

        case speech::VoiceCommand::CMD_FA_SONG: {
            // 发送 - 只有在聊天模式下才能发送
            bsp_display_lock(0);
            if (m_chat_mode && m_send_btn) {
                lv_obj_send_event(m_send_btn, LV_EVENT_CLICKED, this);
            } else {
                ESP_LOGW(TAG, "Cannot send in non-chat mode");
            }
            bsp_display_unlock();
            break;
        }

        case speech::VoiceCommand::CMD_LIAN_JIE_WIFI: {
            // 网络
            bsp_display_lock(0);
            if (m_wifi_btn) {
                lv_obj_send_event(m_wifi_btn, LV_EVENT_CLICKED, this);
            }
            bsp_display_unlock();
            break;
        }

        case speech::VoiceCommand::CMD_GUAN_BI_SHENG_YIN: {
            // 关闭声音
            if (m_voice_enabled) {
                m_voice_enabled = false;
                bsp_display_lock(0);
                lv_obj_set_style_bg_color(m_voice_btn, lv_color_hex(0x9E9E9E), 0);
                bsp_display_unlock();
                who::sys::tts_stop();
                who::sys::tts_speak("语音关闭成功");
            }
            break;
        }

        case speech::VoiceCommand::CMD_DA_KAI_SHENG_YIN: {
            // 打开声音
            if (!m_voice_enabled) {
                m_voice_enabled = true;
                bsp_display_lock(0);
                lv_obj_set_style_bg_color(m_voice_btn, lv_color_hex(0x2196F3), 0);
                bsp_display_unlock();
                who::sys::tts_speak("语音打开成功");
            }
            break;
        }

        case speech::VoiceCommand::CMD_PAI_ZHAO_SHI_BIE: {
            // 帮我拍照并识别 - 相当于 Capture 按钮功能
            if (!m_identify_mode) {
                // 拍照保存并识别
                execute_voice_command(speech::VoiceCommand::CMD_PAI_ZHAO);
                vTaskDelay(pdMS_TO_TICKS(500));
                execute_voice_command(speech::VoiceCommand::CMD_SHI_BIE);
            } else {
                ESP_LOGW(TAG, "Cannot capture in identify mode");
            }
            break;
        }

        case speech::VoiceCommand::CMD_DA_KAI_ASSIST: {
            // 打开助手模式
            if (!m_assist_enabled) {
                m_assist_enabled = true;
                bsp_display_lock(0);
                lv_obj_set_style_bg_color(m_assist_btn, lv_color_hex(0xFF9800), 0);
                bsp_display_unlock();
                ESP_LOGI(TAG, "Assist mode enabled by voice");
                if (m_voice_enabled) who::sys::tts_speak("助手模式已打开");
            }
            break;
        }

        case speech::VoiceCommand::CMD_GUAN_BI_ASSIST: {
            // 关闭助手模式
            if (m_assist_enabled) {
                m_assist_enabled = false;
                m_assist_records.clear();
                m_assist_capture_pending = false;
                bsp_display_lock(0);
                lv_obj_set_style_bg_color(m_assist_btn, lv_color_hex(0x9E9E9E), 0);
                bsp_display_unlock();
                ESP_LOGI(TAG, "Assist mode disabled by voice");
                if (m_voice_enabled) who::sys::tts_speak("助手模式已关闭");
            }
            break;
        }

        default:
            ESP_LOGW(TAG, "Unknown voice command: %d", (int)cmd);
            break;
    }
}

void MyAppUI::update_infer_time(int64_t ms)
{
    if (!m_perf_infer_label) return;
    static char buf[24];
    snprintf(buf, sizeof(buf), "T:%lldms", ms);
    bsp_display_lock(0);
    lv_label_set_text(m_perf_infer_label, buf);
    bsp_display_unlock();
}

void MyAppUI::update_sr_time(int64_t ms)
{
    if (!m_perf_sr_label) return;
    static char buf[24];
    snprintf(buf, sizeof(buf), "SR:%lldms", ms);
    bsp_display_lock(0);
    lv_label_set_text(m_perf_sr_label, buf);
    bsp_display_unlock();
}

} // namespace ui
} // namespace who
