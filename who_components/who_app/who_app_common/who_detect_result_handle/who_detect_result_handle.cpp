#include "who_detect_result_handle.hpp"
#if !BSP_CONFIG_NO_GRAPHIC_LIB
#include "who_lvgl_utils.hpp"
#endif
#include "dl_image_pixel_cvt_dispatch.hpp"

namespace who {
namespace detect {
void draw_detect_results_on_img(const dl::image::img_t &img,
                                const std::list<dl::detect::result_t> &detect_res,
                                const std::vector<std::vector<uint8_t>> &palette)
{
    for (const auto &res : detect_res) {
        dl::image::draw_hollow_rectangle(img, res.box[0], res.box[1], res.box[2], res.box[3], palette[res.category], 2);
        if (!res.keypoint.empty()) {
            assert(res.keypoint.size() == 10);
            for (int i = 0; i < 5; i++) {
                dl::image::draw_point(img, res.keypoint[2 * i], res.keypoint[2 * i + 1], palette[res.category], 3);
            }
        }
    }
}

#if !BSP_CONFIG_NO_GRAPHIC_LIB
void draw_detect_results_on_canvas(lv_obj_t *canvas,
                                   const std::list<dl::detect::result_t> &detect_res,
                                   const std::vector<lv_color_t> &palette,
                                   const char * const *class_names,
                                   const lv_font_t *font)
{
    // 检查canvas是否有效且可见
    if (!canvas || lv_obj_has_flag(canvas, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }

    lv_draw_rect_dsc_t rect_dsc;
    lv_draw_rect_dsc_init(&rect_dsc);
    rect_dsc.bg_opa = LV_OPA_TRANSP;
    rect_dsc.border_width = 2;

    lv_draw_arc_dsc_t arc_dsc;
    lv_draw_arc_dsc_init(&arc_dsc);
    arc_dsc.width = 5;
    arc_dsc.radius = 5;
    arc_dsc.start_angle = 0;
    arc_dsc.end_angle = 360;

    // 标签绘制描述符
    lv_draw_label_dsc_t label_dsc;
    lv_draw_label_dsc_init(&label_dsc);
    label_dsc.color = lv_color_white();
    label_dsc.align = LV_TEXT_ALIGN_LEFT;
    if (font) {
        label_dsc.font = font;
    }

    // 标签背景描述符
    lv_draw_rect_dsc_t bg_dsc;
    lv_draw_rect_dsc_init(&bg_dsc);
    bg_dsc.bg_opa = LV_OPA_70;
    bg_dsc.border_width = 0;
    bg_dsc.radius = 2;

    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);
    lv_area_t coords_rect;
    for (const auto &res : detect_res) {
        coords_rect = {res.box[0], res.box[1], res.box[2], res.box[3]};
        rect_dsc.border_color = palette[res.category];
        lv_draw_rect(&layer, &rect_dsc, &coords_rect);

        // 在矩形框上方绘制物种名称
        if (class_names && font) {
            const char *name = class_names[res.category];
            if (name) {
                lv_point_t txt_size;
                lv_text_get_size(&txt_size, name, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
                int32_t label_w = txt_size.x + 8;   // 左右各4px padding
                int32_t label_h = txt_size.y + 4;    // 上下各2px padding
                int32_t label_x = res.box[0];
                int32_t label_y = res.box[1] - label_h - 2;  // 框上方2px间距
                if (label_y < 0) label_y = 0;

                // 绘制半透明背景
                lv_area_t bg_area = {label_x, label_y, label_x + label_w - 1, label_y + label_h - 1};
                bg_dsc.bg_color = palette[res.category];
                lv_draw_rect(&layer, &bg_dsc, &bg_area);

                // 绘制文字
                lv_area_t label_area = {label_x + 4, label_y + 2, label_x + label_w - 5, label_y + label_h - 3};
                label_dsc.text = name;
                lv_draw_label(&layer, &label_dsc, &label_area);
            }
        }

        if (!res.keypoint.empty()) {
            arc_dsc.color = palette[res.category];
            assert(res.keypoint.size() == 10);
            for (int i = 0; i < 5; i++) {
                arc_dsc.center.x = res.keypoint[2 * i];
                arc_dsc.center.y = res.keypoint[2 * i + 1];
                lv_draw_arc(&layer, &arc_dsc);
            }
        }
    }
    lv_canvas_finish_layer(canvas, &layer);
}
#endif

void print_detect_results(const std::list<dl::detect::result_t> &detect_res)
{
    int i = 0;
    const char *TAG = "detect";
    if (!detect_res.empty()) {
        if (detect_res.begin()->keypoint.empty()) {
            ESP_LOGI(TAG, "----------------------------------------");
        } else {
            ESP_LOGI(
                TAG,
                "---------------------------------------------------------------------------------------------------"
                "---------------------------------------------------");
        }
    }
    for (const auto &r : detect_res) {
        if (r.keypoint.empty()) {
            ESP_LOGI(TAG, "%d, bbox: [%f, %d, %d, %d, %d]", i, r.score, r.box[0], r.box[1], r.box[2], r.box[3]);
        } else {
            assert(r.keypoint.size() == 10);
            ESP_LOGI(TAG,
                     "%d, bbox: [%f, %d, %d, %d, %d], left_eye: [%d, %d], left_mouth: [%d, %d], nose: [%d, %d], "
                     "right_eye: [%d, %d], right_mouth: [%d, %d]",
                     i,
                     r.score,
                     r.box[0],
                     r.box[1],
                     r.box[2],
                     r.box[3],
                     r.keypoint[0],
                     r.keypoint[1],
                     r.keypoint[2],
                     r.keypoint[3],
                     r.keypoint[4],
                     r.keypoint[5],
                     r.keypoint[6],
                     r.keypoint[7],
                     r.keypoint[8],
                     r.keypoint[9]);
        }
        i++;
    }
}
} // namespace detect

namespace lcd_disp {
#if !BSP_CONFIG_NO_GRAPHIC_LIB
WhoDetectResultLCDDisp::WhoDetectResultLCDDisp(task::WhoTask *task,
                                               lv_obj_t *canvas,
                                               const std::vector<std::vector<uint8_t>> &palette,
                                               const char * const *class_names,
                                               const lv_font_t *font) :
    m_task(task), m_res_mutex(xSemaphoreCreateMutex()), m_result(), m_canvas(canvas), m_class_names(class_names), m_font(font)
{
    m_palette = cvt_to_lv_palette(palette);
}
#else
WhoDetectResultLCDDisp::WhoDetectResultLCDDisp(task::WhoTask *task, const std::vector<std::vector<uint8_t>> &palette) :
    m_task(task),
    m_res_mutex(xSemaphoreCreateMutex()),
    m_result(),
    m_rgb888_palette(palette),
    m_rgb565_palette(palette.size(), std::vector<uint8_t>(2))
{
#if CONFIG_IDF_TARGET_ESP32P4
    uint32_t caps = 0;
#else
    uint32_t caps = dl::image::DL_IMAGE_CAP_RGB565_BIG_ENDIAN;
#endif
    for (int i = 0; i < m_rgb888_palette.size(); i++) {
        dl::image::cvt_pix(m_rgb888_palette[i].data(),
                           m_rgb565_palette[i].data(),
                           dl::image::DL_IMAGE_PIX_TYPE_RGB888,
                           dl::image::DL_IMAGE_PIX_TYPE_RGB565,
                           caps);
    }
}
#endif

WhoDetectResultLCDDisp::~WhoDetectResultLCDDisp()
{
    vSemaphoreDelete(m_res_mutex);
}

void WhoDetectResultLCDDisp::save_detect_result(const detect::WhoDetect::result_t &result)
{
    xSemaphoreTake(m_res_mutex, portMAX_DELAY);
    m_results.push(result);
    xSemaphoreGive(m_res_mutex);
}

void WhoDetectResultLCDDisp::lcd_disp_cb(who::cam::cam_fb_t *fb)
{
    if (!m_task->is_active()) {
        return;
    }
    xSemaphoreTake(m_res_mutex, portMAX_DELAY);
    // Try to sync camera frame and result, skip the future result.
    auto compare_timestamp = [](const struct timeval &t1, const struct timeval &t2) -> bool {
        if (t1.tv_sec == t2.tv_sec) {
            return t1.tv_usec < t2.tv_usec;
        }
        return t1.tv_sec < t2.tv_sec;
    };
    struct timeval t1 = fb->timestamp;
    // If detect fps higher than display fps, the result queue may be more than 1. May happen when using lvgl.
    while (!m_results.empty()) {
        detect::WhoDetect::result_t result = m_results.front();
        if (!compare_timestamp(t1, result.timestamp)) {
            m_result = result;
            m_results.pop();
        } else {
            break;
        }
    }
    xSemaphoreGive(m_res_mutex);
#if BSP_CONFIG_NO_GRAPHIC_LIB
    if (fb->format == cam::cam_fb_fmt_t::CAM_FB_FMT_RGB565) {
        detect::draw_detect_results_on_img(*fb, m_result.det_res, m_rgb565_palette);
    } else if (fb->format == cam::cam_fb_fmt_t::CAM_FB_FMT_RGB888) {
        detect::draw_detect_results_on_img(*fb, m_result.det_res, m_rgb888_palette);
    }
#else
    detect::draw_detect_results_on_canvas(m_canvas, m_result.det_res, m_palette, m_class_names, m_font);
#endif
}

void WhoDetectResultLCDDisp::cleanup()
{
    xSemaphoreTake(m_res_mutex, portMAX_DELAY);
    std::queue<detect::WhoDetect::result_t>().swap(m_results);
    m_result = {};
    xSemaphoreGive(m_res_mutex);
}
} // namespace lcd_disp
} // namespace who
