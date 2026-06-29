#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "who_detect_app_lcd.hpp"
#include "model/detect.hpp"
#include "model/coco_classes.hpp"
#include "sys.hpp"
#include "who_lvgl_lcd.hpp"
#include "app/index.hpp"
#include "app/splash.hpp"
#include "app/wifi_manager.hpp"
#include "app/speech_recognizer.hpp"
#include "who_detect.hpp"
#include "who_frame_cap.hpp"
#include "esp_hosted.h"
#include <cstdio>

// 声明自定义中文字体
LV_FONT_DECLARE(lv_font_alibabapuhuiti_light_16)

using namespace who::frame_cap;
using namespace who::app;
using namespace who::sys;
using namespace who::lcd;
using namespace who::ui;
using namespace who::detect;

// 全局 UI 实例指针，供回调函数更新界面
static MyAppUI *g_app_ui = nullptr;

// 自定义检测应用：将检测结果同步到 UI
class MyDetectAppLCD : public WhoDetectAppLCD {
public:
    MyDetectAppLCD(const std::vector<std::vector<uint8_t>> &palette,
                   WhoFrameCap *frame_cap,
                   WhoFrameCapNode *lcd_disp_frame_cap_node,
                   lv_obj_t *parent,
                   const char * const *class_names = nullptr,
                   const lv_font_t *font = nullptr) :
        WhoDetectAppLCD(palette, frame_cap, lcd_disp_frame_cap_node, parent, class_names, font)
    {
    }

protected:
    // 将检测结果同步到右侧信息面板
    void detect_result_cb(const WhoDetect::result_t &result) override
    {
        // 调用父类方法保存结果（用于绘制检测框）
        WhoDetectAppLCD::detect_result_cb(result);

        // 非识物模式下才更新 UI 检测结果
        if (g_app_ui && !g_app_ui->is_identify_mode()) {
            update_result_ui(result);
            // 送入智能识别模块做进一步分析
            g_app_ui->feed_detect_result(result.det_res);
        }
    }

private:
    void update_result_ui(const WhoDetect::result_t &result)
    {
        static char text_buf[512];
        char *p = text_buf;
        int len = sizeof(text_buf);
        int written = 0;

        // 遍历检测结果，输出每个目标的类别与置信度
        int idx = 0;
        for (const auto &res : result.det_res) {
            if (len <= 0) break;

            written = snprintf(p, len, "Object %d:\n  Cat: %d\n  Score: %.2f\n",
                              idx, res.category, res.score);
            p += written;
            len -= written;
            idx++;
        }

        if (result.det_res.empty() && len > 0) {
            snprintf(p, len, "No objects detected\n");
        }

        g_app_ui->update_detect_result(result.det_res.size(), text_buf);
        g_app_ui->update_infer_time(result.infer_time_ms);
    }
};

void run_detect_lcd()
{
    esp_log_level_set("tts_parser", ESP_LOG_WARN);
    esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_WARN);

    // SD 卡挂载失败不阻塞启动，仅记录警告
    esp_err_t ret = bsp_sdcard_mount();
    if (ret != ESP_OK) {
        ESP_LOGW("main", "SD card mount failed (0x%x), continuing without SD card", ret);
    }

    // ====== 第一阶段：后台加载模型======

    // 1. 初始化 LVGL 显示框架
    lvgl_manual_init();
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // 2. 创建摄像头帧捕获管线（检测模型的图像输入源）
    WhoFrameCapNode *lcd_disp_frame_cap_node = nullptr;
    auto frame_cap = get_lcd_mipi_csi_ppa_frame_cap_pipeline(&lcd_disp_frame_cap_node);

    // 3. 加载视觉检测模型
    dl::detect::Detect *detect_model = get_detect_model();

    // 4. 初始化 TTS
    who::sys::tts_init();

    // 5. 初始化语音识别器
    auto speech_recognizer = who::speech::SpeechRecognizer::get_instance();
    bool speech_ready = speech_recognizer->init();

    // ====== 第二阶段：展示启动画面（模型已就绪）======

    // 6. 创建 UI
    MyAppUI *app_ui = new MyAppUI();
    app_ui->init();
    g_app_ui = app_ui;
    app_ui->set_frame_cap_node(lcd_disp_frame_cap_node);
    app_ui->set_detect_model(detect_model);

    // 7. 创建启动画面，覆盖在主界面之上
    SplashScreen *splash = new SplashScreen();
    splash->init();
    splash->set_hint_text("Preparing...");
    splash->step_progress(20);

    bsp_display_backlight_on();

    // ====== 第三阶段：初始化外设与服务 ======

    // 8. 初始化图片管理器
    splash->set_hint_text("Loading model...");
    app_ui->init_image_manager();
    splash->step_progress(20);

    // 9. 初始化 WiFi 并设置状态回调
    auto wifi_mgr = who::wifi::WifiManager::get_instance();
    if (wifi_mgr->init()) {
        wifi_mgr->set_status_callback([app_ui](who::wifi::WifiState state) {
            app_ui->update_wifi_button_state(state);
        });
    }
    splash->set_hint_text("Initializing WiFi...");
    splash->step_progress(15);

    // 10. 创建检测应用
    lv_obj_t *camera_container = app_ui->get_camera_container();
    auto detect_app = new MyDetectAppLCD({{255, 0, 0}}, frame_cap, lcd_disp_frame_cap_node, camera_container, coco_classes, &lv_font_alibabapuhuiti_light_16);
    detect_app->set_model(detect_model);

    // 11. 注册识物模式的摄像头暂停/恢复回调
    app_ui->set_camera_pause_cb([detect_app]() {
        detect_app->pause();
    });
    app_ui->set_camera_resume_cb([detect_app]() {
        detect_app->resume();
    });

    // 12. 启动检测应用
    detect_app->run();
    splash->set_hint_text("Starting services...");
    splash->step_progress(25);

    // 13. 初始化外接按钮（GPIO 23，高电平触发）
    if (who::sys::button_init(GPIO_NUM_23)) {
        who::sys::button_set_callback([app_ui]() {
            if (app_ui->is_chat_mode()) {
                app_ui->execute_voice_command(who::speech::VoiceCommand::CMD_FA_SONG);
            } else if (app_ui->is_identify_mode()) {
                app_ui->execute_voice_command(who::speech::VoiceCommand::CMD_SHI_BIE);
            } else {
                app_ui->execute_voice_command(who::speech::VoiceCommand::CMD_PAI_ZHAO);
                vTaskDelay(pdMS_TO_TICKS(500));
                app_ui->execute_voice_command(who::speech::VoiceCommand::CMD_SHI_BIE);
            }
        });
        who::sys::button_start();
    }

    splash->set_hint_text("Almost ready...");
    splash->step_progress(20);
    vTaskDelay(pdMS_TO_TICKS(500));  // 等待进度条动画播完
    splash->dismiss();
    delete splash;

    // 14. 启动语音识别（模型已加载，开始监听）
    if (speech_ready) {
        speech_recognizer->set_command_callback([app_ui](who::speech::VoiceCommand cmd, int64_t sr_time_ms) {
            app_ui->update_sr_time(sr_time_ms);
            app_ui->execute_voice_command(cmd);
        });
        speech_recognizer->start();
    } else {
        ESP_LOGW("main", "Speech recognizer initialization failed, voice control disabled");
    }
}

extern "C" void app_main(void)
{
    vTaskPrioritySet(xTaskGetCurrentTaskHandle(), 5);

    // 初始化 ESP-Hosted（ESP32-C6 WiFi 协处理器，通过 SDIO 通信）
    ESP_ERROR_CHECK(esp_hosted_init());
    run_detect_lcd();
}