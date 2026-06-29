#include "index.hpp"
#include "sys.hpp"
#include "esp_timer.h"
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>

// 声明自定义中文字体
LV_FONT_DECLARE(lv_font_alibabapuhuiti_light_16)

static const char *TAG = "MyAppUI";

namespace who {
namespace ui {

// Capture 按钮点击回调
void MyAppUI::onCaptureClicked(lv_event_t *e)
{
    MyAppUI *app = (MyAppUI*)lv_event_get_user_data(e);
    if (!app) return;

    struct CaptureParams { MyAppUI* app; };
    CaptureParams* params = new CaptureParams{app};

    xTaskCreatePinnedToCoreWithCaps([](void* arg) {
        CaptureParams* p = static_cast<CaptureParams*>(arg);
        p->app->execute_voice_command(speech::VoiceCommand::CMD_PAI_ZHAO);
        vTaskDelay(pdMS_TO_TICKS(500));
        p->app->execute_voice_command(speech::VoiceCommand::CMD_SHI_BIE);
        delete p;
        vTaskDelete(nullptr);
    }, "capture_task", 4 * 1024, params, 5, nullptr, 0, MALLOC_CAP_SPIRAM);
}

// Identify/Back 按钮点击回调 - 切换识别浏览模式
void MyAppUI::onIdentifyClicked(lv_event_t *e)
{
    MyAppUI *app = (MyAppUI*)lv_event_get_user_data(e);
    if (!app) return;

    bsp_display_lock(0);

    lv_obj_t *label = lv_obj_get_child(app->m_identify_btn, 0);
    const char *text = lv_label_get_text(label);

    if (strcmp(text, "科普界面") == 0) {
        lv_label_set_text(label, "浏览界面");
        lv_obj_clear_flag(app->m_browse_container, LV_OBJ_FLAG_HIDDEN);
        app->switch_to_identify_mode();
    } else {
        lv_label_set_text(label, "科普界面");
        lv_obj_add_flag(app->m_browse_container, LV_OBJ_FLAG_HIDDEN);
        app->switch_to_camera_mode();
    }

    bsp_display_unlock();
}

// Pre 上一张按钮点击回调
void MyAppUI::onPreClicked(lv_event_t *e)
{
    MyAppUI *app = (MyAppUI*)lv_event_get_user_data(e);
    if (!app || !app->m_image_manager) return;

    app->m_image_manager->prev_image();
    app->update_image_count_label();
    app->update_image_display();
}

// Start 开始按钮点击回调 - 对当前图片执行检测
void MyAppUI::onStartClicked(lv_event_t *e)
{
    MyAppUI *app = (MyAppUI*)lv_event_get_user_data(e);
    if (!app) return;

    bsp_display_lock(0);
    app->clear_result_textareas();
    lv_obj_add_flag(app->m_ai_chat_btn, LV_OBJ_FLAG_HIDDEN);
    if (app->m_chat_mode) {
        app->m_chat_mode = false;
        lv_obj_add_flag(app->m_chat_container, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(app->m_keyboard, LV_OBJ_FLAG_HIDDEN);
        if (app->m_ime_pinyin) {
            lv_obj_t *panel = lv_ime_pinyin_get_cand_panel(app->m_ime_pinyin);
            if (panel) lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_clear_flag(app->m_browse_container, LV_OBJ_FLAG_HIDDEN);
        lv_obj_t *count_delete_container = lv_obj_get_parent(app->m_image_count_label);
        if (count_delete_container) {
            lv_obj_clear_flag(count_delete_container, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_t *label = lv_obj_get_child(app->m_ai_chat_btn, 0);
        if (label) {
            lv_label_set_text(label, "Ai聊天");
        }
        lv_obj_set_size(app->m_result_container, 400, 420);
        uint16_t sel = lv_dropdown_get_selected(app->m_llm_dropdown);
        auto llm = who::llm::LLMClient::get_instance();
        if (llm) llm->set_mode(sel == 1 ? who::llm::LLMMode::VISION : who::llm::LLMMode::LANGUAGE);
    }
    bsp_display_unlock();

    app->m_llm_task_cancelled = false;

    if (!app->m_image_manager) {
        ESP_LOGE(TAG, "Image manager not initialized");
        return;
    }

    if (!app->m_detect_model) {
        ESP_LOGE(TAG, "Detect model not set");
        bsp_display_lock(0);
        lv_label_set_text(app->m_result_title_label, "No model loaded!");
        bsp_display_unlock();
        return;
    }

    std::string current_path = app->m_image_manager->get_current_image_path();
    if (current_path.empty()) {
        ESP_LOGE(TAG, "No image selected");
        bsp_display_lock(0);
        lv_label_set_text(app->m_result_title_label, "No image!\nCapture first");
        bsp_display_unlock();
        return;
    }

    bsp_display_lock(0);
    lv_label_set_text(app->m_result_title_label, "Detecting...\nPlease wait");
    bsp_display_unlock();

    if (app->m_voice_enabled) who::sys::tts_speak("正在识别");

    app->perform_single_image_detect();
}

// Next 下一张按钮点击回调
void MyAppUI::onNextClicked(lv_event_t *e)
{
    MyAppUI *app = (MyAppUI*)lv_event_get_user_data(e);
    if (!app || !app->m_image_manager) return;

    app->m_image_manager->next_image();
    app->update_image_count_label();
    app->update_image_display();
}

// Delete 删除按钮点击回调 - 删除当前图片
void MyAppUI::onDeleteClicked(lv_event_t *e)
{
    MyAppUI *app = (MyAppUI*)lv_event_get_user_data(e);
    if (!app || !app->m_image_manager) return;

    bool deleted = app->m_image_manager->delete_current_image();

    if (deleted) {
        app->update_image_count_label();
        app->update_image_display();
        bsp_display_lock(0);
        lv_label_set_text(app->m_result_title_label, "Image deleted!");
        bsp_display_unlock();
    } else {
        ESP_LOGE(TAG, "Failed to delete image");
        bsp_display_lock(0);
        lv_label_set_text(app->m_result_title_label, "Delete failed!\nNo images left");
        bsp_display_unlock();
    }
}

// WiFi 按钮点击回调
void MyAppUI::onWifiClicked(lv_event_t *e)
{
    MyAppUI *app = (MyAppUI*)lv_event_get_user_data(e);
    if (!app) return;

    auto wifi_mgr = who::wifi::WifiManager::get_instance();
    if (wifi_mgr) {
        wifi_mgr->toggle_connection();
    }
}

// Voice 开关按钮点击回调
void MyAppUI::onVoiceClicked(lv_event_t *e)
{
    MyAppUI *app = (MyAppUI*)lv_event_get_user_data(e);
    if (!app) return;

    app->m_voice_enabled = !app->m_voice_enabled;

    lv_obj_t *vl = lv_obj_get_child(app->m_voice_btn, 0);
    if (app->m_voice_enabled) {
        lv_obj_set_style_bg_color(app->m_voice_btn, lv_color_hex(0x2196F3), 0);
        if (vl) lv_label_set_text(vl, "关闭声音");
    } else {
        lv_obj_set_style_bg_color(app->m_voice_btn, lv_color_hex(0x9E9E9E), 0);
        if (vl) lv_label_set_text(vl, "打开声音");
        who::sys::tts_stop();
    }
}

// Assist 开关按钮点击回调
void MyAppUI::onAssistClicked(lv_event_t *e)
{
    MyAppUI *app = (MyAppUI*)lv_event_get_user_data(e);
    if (!app) return;

    app->m_assist_enabled = !app->m_assist_enabled;

    if (app->m_assist_enabled) {
        lv_obj_set_style_bg_color(app->m_assist_btn, lv_color_hex(0xFF9800), 0);
        ESP_LOGI(TAG, "Assist mode enabled");
        if (app->m_voice_enabled) who::sys::tts_speak("助手模式已打开");
    } else {
        lv_obj_set_style_bg_color(app->m_assist_btn, lv_color_hex(0x9E9E9E), 0);
        app->m_assist_records.clear();
        app->m_assist_capture_pending = false;
        ESP_LOGI(TAG, "Assist mode disabled");
        if (app->m_voice_enabled) who::sys::tts_speak("助手模式已关闭");
    }
}

// 设置按钮点击回调 - 显示/隐藏设置容器
void MyAppUI::onSettingsClicked(lv_event_t *e)
{
    MyAppUI *app = (MyAppUI*)lv_event_get_user_data(e);
    if (!app) return;

    app->m_settings_open = !app->m_settings_open;

    if (app->m_settings_open) {
        lv_obj_update_layout(app->m_settings_btn);
        lv_obj_align_to(app->m_settings_container, app->m_settings_btn,
                        LV_ALIGN_OUT_BOTTOM_RIGHT, 0, 8);
        lv_obj_clear_flag(app->m_settings_container, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(app->m_settings_container);
        app->m_settings_overlay = lv_obj_create(lv_scr_act());
        lv_obj_remove_style_all(app->m_settings_overlay);
        lv_obj_set_size(app->m_settings_overlay, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_bg_opa(app->m_settings_overlay, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(app->m_settings_overlay, 0, 0);
        lv_obj_clear_flag(app->m_settings_overlay, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(app->m_settings_overlay, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(app->m_settings_overlay, onOverlayClicked, LV_EVENT_CLICKED, app);
        lv_obj_move_foreground(app->m_settings_overlay);
        lv_obj_move_foreground(app->m_settings_container);
    } else {
        if (app->m_settings_overlay) {
            lv_obj_del(app->m_settings_overlay);
            app->m_settings_overlay = nullptr;
        }
        lv_dropdown_close(app->m_llm_dropdown);
        lv_obj_add_flag(app->m_settings_container, LV_OBJ_FLAG_HIDDEN);
    }
}

// 覆盖层点击回调 - 点击非设置容器区域时关闭
void MyAppUI::onOverlayClicked(lv_event_t *e)
{
    MyAppUI *app = (MyAppUI*)lv_event_get_user_data(e);
    if (!app || !app->m_settings_open) return;

    lv_point_t point;
    lv_indev_get_point(lv_indev_get_act(), &point);

    lv_area_t cont_coords;
    lv_obj_get_coords(app->m_settings_container, &cont_coords);
    if (point.x >= cont_coords.x1 && point.x <= cont_coords.x2 &&
        point.y >= cont_coords.y1 && point.y <= cont_coords.y2) {
        return;
    }

    lv_area_t btn_coords;
    lv_obj_get_coords(app->m_settings_btn, &btn_coords);
    if (point.x >= btn_coords.x1 && point.x <= btn_coords.x2 &&
        point.y >= btn_coords.y1 && point.y <= btn_coords.y2) {
        lv_obj_t *overlay = app->m_settings_overlay;
        app->m_settings_overlay = nullptr;
        lv_obj_del(overlay);
        lv_obj_send_event(app->m_settings_btn, LV_EVENT_CLICKED, nullptr);
        return;
    }

    if (app->m_settings_overlay) {
        lv_obj_del(app->m_settings_overlay);
        app->m_settings_overlay = nullptr;
    }
    lv_dropdown_close(app->m_llm_dropdown);
    app->m_settings_open = false;
    lv_obj_add_flag(app->m_settings_container, LV_OBJ_FLAG_HIDDEN);
}

// LLM 下拉菜单选择回调
void MyAppUI::onLLMDropdownChanged(lv_event_t *e)
{
    MyAppUI *app = (MyAppUI*)lv_event_get_user_data(e);
    if (!app) return;

    lv_obj_t *dropdown = (lv_obj_t *)lv_event_get_target(e);
    uint16_t selected = lv_dropdown_get_selected(dropdown);

    auto llm_client = who::llm::LLMClient::get_instance();
    if (llm_client) {
        llm_client->init();
        if (selected == 0) {
            ESP_LOGI(TAG, "Language mode selected");
            llm_client->set_mode(who::llm::LLMMode::LANGUAGE);
        } else if (selected == 1) {
            llm_client->set_mode(who::llm::LLMMode::VISION);
        }
    }
}

// AiChat 按钮点击回调 - 切换聊天模式
void MyAppUI::onAiChatClicked(lv_event_t *e)
{
    MyAppUI *app = (MyAppUI*)lv_event_get_user_data(e);
    if (!app) return;

    bsp_display_lock(0);

    lv_obj_t *label = lv_obj_get_child(app->m_ai_chat_btn, 0);
    const char *text = lv_label_get_text(label);

    if (strcmp(text, "Ai聊天") == 0) {
        app->m_chat_mode = true;
        lv_label_set_text(label, "Back");
        lv_obj_add_flag(app->m_browse_container, LV_OBJ_FLAG_HIDDEN);
        lv_obj_t *count_delete_container = lv_obj_get_parent(app->m_image_count_label);
        if (count_delete_container) {
            lv_obj_add_flag(count_delete_container, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_clear_flag(app->m_chat_container, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(app->m_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(app->m_result_container, 400, 420);
    } else {
        app->m_chat_mode = false;
        uint16_t selected = lv_dropdown_get_selected(app->m_llm_dropdown);
        auto llm_resume = who::llm::LLMClient::get_instance();
        if (llm_resume) {
            llm_resume->set_mode(selected == 1 ? who::llm::LLMMode::VISION : who::llm::LLMMode::LANGUAGE);
        }
        lv_label_set_text(label, "Ai聊天");
        lv_obj_add_flag(app->m_chat_container, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(app->m_keyboard, LV_OBJ_FLAG_HIDDEN);
        if (app->m_ime_pinyin) {
            lv_obj_t *panel = lv_ime_pinyin_get_cand_panel(app->m_ime_pinyin);
            if (panel) lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_set_size(app->m_result_container, 400, 420);
        lv_obj_clear_flag(app->m_browse_container, LV_OBJ_FLAG_HIDDEN);
        lv_obj_t *count_delete_container = lv_obj_get_parent(app->m_image_count_label);
        if (count_delete_container) {
            lv_obj_clear_flag(count_delete_container, LV_OBJ_FLAG_HIDDEN);
        }
    }

    bsp_display_unlock();
}

// Send 按钮点击回调 - 发送用户消息并获取AI回复
void MyAppUI::onSendClicked(lv_event_t *e)
{
    MyAppUI *app = (MyAppUI*)lv_event_get_user_data(e);
    if (!app) return;

    const char *text = lv_textarea_get_text(app->m_chat_input);
    if (!text || strlen(text) == 0) {
        ESP_LOGW(TAG, "Empty message, ignoring");
        return;
    }

    std::string user_message(text);
    std::string detection_context = app->m_detection_context;

    bsp_display_lock(0);
    lv_textarea_set_text(app->m_chat_input, "");
    lv_obj_add_flag(app->m_keyboard, LV_OBJ_FLAG_HIDDEN);
    if (app->m_ime_pinyin) {
        lv_obj_t *panel = lv_ime_pinyin_get_cand_panel(app->m_ime_pinyin);
        if (panel) lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t *user_ta = lv_textarea_create(app->m_result_container);
    lv_obj_set_size(user_ta, 280, LV_SIZE_CONTENT);
    lv_textarea_set_one_line(user_ta, false);
    lv_textarea_set_max_length(user_ta, 0);
    lv_obj_remove_style(user_ta, nullptr, LV_PART_SCROLLBAR);
    lv_obj_add_flag(user_ta, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_set_scrollbar_mode(user_ta, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_text_align(user_ta, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_bg_color(user_ta, lv_color_hex(0x2196F3), 0);
    lv_obj_set_style_text_color(user_ta, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(user_ta, &lv_font_alibabapuhuiti_light_16, 0);
    lv_obj_set_style_border_width(user_ta, 1, 0);
    lv_obj_set_style_border_color(user_ta, lv_color_hex(0x1976D2), 0);
    lv_obj_set_style_radius(user_ta, 6, 0);
    lv_obj_set_style_pad_all(user_ta, 8, 0);

    std::string user_display = "我：" + user_message;
    lv_textarea_set_text(user_ta, user_display.c_str());
    app->m_result_textareas.push_back(user_ta);

    lv_obj_t *ai_ta = lv_textarea_create(app->m_result_container);
    lv_obj_set_size(ai_ta, 370, LV_SIZE_CONTENT);
    lv_textarea_set_one_line(ai_ta, false);
    lv_textarea_set_max_length(ai_ta, 0);
    lv_obj_remove_style(ai_ta, nullptr, LV_PART_SCROLLBAR);
    lv_obj_add_flag(ai_ta, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_set_scrollbar_mode(ai_ta, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_text_align(ai_ta, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_bg_color(ai_ta, lv_color_hex(0x4CAF50), 0);
    lv_obj_set_style_text_color(ai_ta, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(ai_ta, &lv_font_alibabapuhuiti_light_16, 0);
    lv_obj_set_style_border_width(ai_ta, 1, 0);
    lv_obj_set_style_border_color(ai_ta, lv_color_hex(0x388E3C), 0);
    lv_obj_set_style_radius(ai_ta, 6, 0);
    lv_obj_set_style_pad_all(ai_ta, 8, 0);
    lv_textarea_set_text(ai_ta, "AI：思考中...");
    app->m_result_textareas.push_back(ai_ta);

    bsp_display_unlock();

    struct ChatTaskParams {
        std::string message;
        std::string detection_context;
        lv_obj_t *ai_textarea;
        MyAppUI* app;
    };
    ChatTaskParams *params = new ChatTaskParams{user_message, detection_context, ai_ta, app};

    llm_worker_init();
    LLMTaskMsg msg = {[](void *arg) {
        ChatTaskParams *p = static_cast<ChatTaskParams *>(arg);

        auto llm = who::llm::LLMClient::get_instance();
        llm->init();

        llm->set_mode(who::llm::LLMMode::AICHAT);
        if (!p->detection_context.empty()) {
            llm->set_detection_context(p->detection_context.c_str());
        } else {
            llm->set_detection_context("");
        }

        int64_t t_chat_start = esp_timer_get_time();
        std::string response = llm->send_message_sync(p->message.c_str());
        int64_t t_chat_end = esp_timer_get_time();
        p->app->update_infer_time((t_chat_end - t_chat_start) / 1000);

        bsp_display_lock(0);
        if (response.empty()) {
            lv_textarea_set_text(p->ai_textarea, "AI：回复失败，请重试");
            ESP_LOGE(TAG, "AI Chat response is empty");
        } else {
            std::string ai_display = "AI：" + response;
            lv_textarea_set_text(p->ai_textarea, ai_display.c_str());
        }
        bsp_display_unlock();

        delete p;
    }, params};
    llm_worker_send(&msg);
}

} // namespace ui
} // namespace who
