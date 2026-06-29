#include "splash.hpp"
#include "dl_image_jpeg.hpp"
#include "bsp/esp-bsp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <cstring>
#include <algorithm>

static const char *TAG = "SplashScreen";

namespace who {
namespace ui {

SplashScreen::SplashScreen()
    : m_cont(nullptr)
    , m_canvas(nullptr)
    , m_hint_label(nullptr)
    , m_progress_bar(nullptr)
    , m_progress_label(nullptr)
    , m_image_buffer(nullptr)
{
}

SplashScreen::~SplashScreen()
{
    dismiss();
}

bool SplashScreen::init()
{
    bsp_display_lock(0);

    // 1. 全屏覆盖层 
    m_cont = lv_obj_create(lv_scr_act());
    lv_obj_set_size(m_cont, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_pos(m_cont, 0, 0);
    lv_obj_set_style_bg_color(m_cont, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(m_cont, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(m_cont, 0, 0);
    lv_obj_set_style_pad_all(m_cont, 0, 0);
    lv_obj_clear_flag(m_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(m_cont, 0, 0);

    // 2. 垂直居中布局容器
    lv_obj_t *outer = lv_obj_create(m_cont);
    lv_obj_set_size(outer, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_pos(outer, 0, 0);
    lv_obj_set_flex_flow(outer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(outer, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(outer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(outer, 0, 0);
    lv_obj_set_style_pad_all(outer, 0, 0);
    lv_obj_set_style_pad_row(outer, 14, 0);

    // 3. 图片 Canvas（展示 ai.jpg）
    m_canvas = lv_canvas_create(outer);
    lv_obj_set_size(m_canvas, IMAGE_W, IMAGE_H);
    lv_obj_set_style_pad_bottom(m_canvas, 14, 0);
    lv_obj_clear_flag(m_canvas, LV_OBJ_FLAG_SCROLLABLE);

    m_image_buffer = (uint8_t *)heap_caps_malloc(IMAGE_W * IMAGE_H * 2, MALLOC_CAP_SPIRAM);
    if (m_image_buffer) {
        memset(m_image_buffer, 0xFF, IMAGE_W * IMAGE_H * sizeof(uint16_t));
        lv_canvas_set_buffer(m_canvas, m_image_buffer, IMAGE_W, IMAGE_H, LV_COLOR_FORMAT_NATIVE);
    } else {
        ESP_LOGE(TAG, "Failed to allocate SPIRAM buffer");
    }

    // 4. 从 SD 卡读取并解码 ai.jpg
    dl::image::jpeg_img_t jpeg_img = dl::image::read_jpeg(IMAGE_PATH);
    if (jpeg_img.data) {
        dl::image::img_t img = dl::image::sw_decode_jpeg(jpeg_img, dl::image::DL_IMAGE_PIX_TYPE_RGB565LE);
        heap_caps_free(jpeg_img.data);

        if (img.data && m_image_buffer) {
            int copy_w = std::min(static_cast<int>(img.width), IMAGE_W);
            int copy_h = std::min(static_cast<int>(img.height), IMAGE_H);
            memset(m_image_buffer, 0xFF, IMAGE_W * IMAGE_H * sizeof(uint16_t));
            int offset_x = (IMAGE_W - copy_w) / 2;
            int offset_y = (IMAGE_H - copy_h) / 2;

            for (int y = 0; y < copy_h; y++) {
                uint8_t *src = (uint8_t *)img.data + y * img.width * 2;
                uint8_t *dst = m_image_buffer + (y + offset_y) * IMAGE_W * 2 + offset_x * 2;
                memcpy(dst, src, copy_w * 2);
            }

            lv_canvas_set_buffer(m_canvas, m_image_buffer, IMAGE_W, IMAGE_H, LV_COLOR_FORMAT_NATIVE);
            ESP_LOGI(TAG, "Splash image loaded: %s (%dx%d)", IMAGE_PATH, img.width, img.height);
        }

        if (img.data) {
            heap_caps_free(img.data);
        }
    } else {
        ESP_LOGW(TAG, "Cannot read splash image %s, showing dark placeholder", IMAGE_PATH);
    }

    // 5. Zivoo 名称和提示文本（同一行，左右分布）
    lv_obj_t *zivoo_row = lv_obj_create(outer);
    lv_obj_set_size(zivoo_row, IMAGE_W, LV_SIZE_CONTENT);
    lv_obj_clear_flag(zivoo_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(zivoo_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(zivoo_row, 0, 0);
    lv_obj_set_style_pad_all(zivoo_row, 0, 0);
    lv_obj_set_flex_flow(zivoo_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(zivoo_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(zivoo_row, 14, 0);

    lv_obj_t *zivoo_label = lv_label_create(zivoo_row);
    lv_label_set_text(zivoo_label, "Zivoo");
    lv_obj_set_style_text_color(zivoo_label, lv_color_hex(0x333333), 0);
    lv_obj_set_style_text_font(zivoo_label, &lv_font_montserrat_28, 0);

    m_hint_label = lv_label_create(zivoo_row);
    lv_label_set_text(m_hint_label, "Initializing...");
    lv_obj_set_style_text_color(m_hint_label, lv_color_hex(0x666666), 0);
    lv_obj_set_style_text_font(m_hint_label, &lv_font_montserrat_20, 0);

    // 6. 进度条
    m_progress_bar = lv_bar_create(outer);
    lv_obj_set_width(m_progress_bar, IMAGE_W);
    lv_obj_set_height(m_progress_bar, 16);
    lv_bar_set_range(m_progress_bar, 0, 100);
    lv_bar_set_value(m_progress_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(m_progress_bar, 8, 0);
    lv_obj_set_style_bg_color(m_progress_bar, lv_color_hex(0xE0E0E0), 0);
    lv_obj_set_style_bg_opa(m_progress_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(m_progress_bar, 0, 0);
    lv_obj_set_style_pad_all(m_progress_bar, 0, 0);
    // 进度条指示器（蓝色）
    lv_obj_set_style_bg_color(m_progress_bar, lv_color_hex(0x4A90D9), LV_PART_INDICATOR);
    lv_obj_set_style_radius(m_progress_bar, 8, LV_PART_INDICATOR);

    // 8. 百分比文本
    m_progress_label = lv_label_create(outer);
    lv_label_set_text(m_progress_label, "0%");
    lv_obj_set_style_text_color(m_progress_label, lv_color_hex(0x555555), 0);

    bsp_display_unlock();

    ESP_LOGI(TAG, "Splash screen initialized");
    return true;
}

void SplashScreen::step_progress(int count, int delay_ms)
{
    if (!m_progress_bar || !m_progress_label) return;

    for (int i = 0; i < count && m_percent < 100; i++) {
        m_percent++;
        bsp_display_lock(0);
        lv_bar_set_value(m_progress_bar, m_percent, LV_ANIM_OFF);
        static char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", m_percent);
        lv_label_set_text(m_progress_label, buf);
        bsp_display_unlock();
        if (delay_ms > 0) vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

void SplashScreen::set_hint_text(const char *text)
{
    if (!m_hint_label) return;
    bsp_display_lock(0);
    lv_label_set_text(m_hint_label, text);
    bsp_display_unlock();
}

void SplashScreen::dismiss()
{
    if (!m_cont) return;

    bsp_display_lock(0);
    lv_obj_delete(m_cont);
    m_cont = nullptr;
    m_canvas = nullptr;
    m_hint_label = nullptr;
    m_progress_bar = nullptr;
    m_progress_label = nullptr;
    bsp_display_unlock();

    if (m_image_buffer) {
        heap_caps_free(m_image_buffer);
        m_image_buffer = nullptr;
    }

    ESP_LOGI(TAG, "Splash screen dismissed");
}

} // namespace ui
} // namespace who
