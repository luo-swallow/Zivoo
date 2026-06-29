#pragma once

#include "dl_detect_base.hpp"
#include "dl_image_define.hpp"
#include "dl_image_draw.hpp"
#include "dl_image_jpeg.hpp"
#include "who_cam_define.hpp"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_err.h"
#include <list>
#include <vector>
#include <string>
#include <ctime>

namespace who {
namespace detect {

/**
 * @brief 单图检测模块 - 从SD卡JPEG图片执行YOLO11检测（header-only实现）
 */
class SingleImageDetect {
public:
    static std::list<dl::detect::result_t> detect_from_jpeg(
        const char *jpeg_path,
        dl::detect::Detect *model)
    {
        if (!model) {
            ESP_LOGE("SingleImageDetect", "Model is nullptr");
            return {};
        }

        dl::image::jpeg_img_t jpeg_img = dl::image::read_jpeg(jpeg_path);
        if (!jpeg_img.data) {
            ESP_LOGE("SingleImageDetect", "Failed to read JPEG file");
            return {};
        }

        dl::image::img_t img = dl::image::sw_decode_jpeg(jpeg_img, dl::image::DL_IMAGE_PIX_TYPE_RGB888);
        heap_caps_free(jpeg_img.data);

        if (!img.data) {
            ESP_LOGE("SingleImageDetect", "Failed to decode JPEG");
            return {};
        }

        auto &results = model->run(img);

        // 修复：释放解码后的图像内存
        heap_caps_free(img.data);

        return results;
    }

    static void draw_results_on_img(
        dl::image::img_t &img,
        const std::list<dl::detect::result_t> &results,
        const std::vector<std::vector<uint8_t>> &palette)
    {
        if (img.pix_quant()) {
            ESP_LOGE("SingleImageDetect", "Cannot draw on quantized image");
            return;
        }

        int col_step = img.col_step();
        for (const auto &res : results) {
            const std::vector<uint8_t> &color = (res.category < palette.size()) ?
                palette[res.category] : palette[0];

            if (color.size() != col_step) {
                ESP_LOGW("SingleImageDetect", "Color size mismatch, using default red");
                std::vector<uint8_t> default_color = {255, 0, 0};
                if (col_step == 2) {
                    default_color = {0, 0, 248};
                }
                dl::image::draw_hollow_rectangle(img, res.box[0], res.box[1], res.box[2], res.box[3],
                                                default_color, 2);
            } else {
                dl::image::draw_hollow_rectangle(img, res.box[0], res.box[1], res.box[2], res.box[3],
                                                color, 2);
            }
        }
    }

    static std::list<dl::detect::result_t> process_single_image(
        const char *jpeg_path,
        dl::detect::Detect *model,
        const std::vector<std::vector<uint8_t>> &palette)
    {
        dl::image::jpeg_img_t jpeg_img = dl::image::read_jpeg(jpeg_path);
        if (!jpeg_img.data) {
            ESP_LOGE("SingleImageDetect", "Failed to read JPEG: %s", jpeg_path);
            return {};
        }

        dl::image::img_t img = dl::image::sw_decode_jpeg(jpeg_img, dl::image::DL_IMAGE_PIX_TYPE_RGB888);
        heap_caps_free(jpeg_img.data);

        if (!img.data) {
            ESP_LOGE("SingleImageDetect", "Failed to decode JPEG");
            return {};
        }

        auto results = model->run(img);
        if (!results.empty()) {
            draw_results_on_img(img, results, palette);
        }

        heap_caps_free(img.data);

        return results;
    }
};

} // namespace detect

namespace image {

/**
 * @brief 图片管理器 - 管理SD卡captures文件夹中的图片
 */
class ImageManager {
public:
    ImageManager();
    ~ImageManager();

    void init();
    void scan_capture_folder();
    std::vector<std::string>& get_image_list();
    int get_image_count();
    int get_current_index();
    bool prev_image();
    bool next_image();
    bool delete_current_image();
    std::string get_current_image_path();
    std::string get_current_image_name();
    std::string save_capture_frame(who::cam::cam_fb_t *fb);
    std::string get_result_path(const std::string &capture_path);
    std::string get_result_path_by_categories(const std::list<dl::detect::result_t> &results);
    void clear_results_folder();

    static constexpr const char *CAPTURE_DIR = "/sdcard/captures";
    static constexpr const char *RESULT_DIR = "/sdcard/results";

private:
    std::vector<std::string> m_image_list;
    int m_current_index;
    int m_capture_counter;

    void ensure_folder_exists(const char *path);
    std::string generate_timestamp_filename();
};

} // namespace image
} // namespace who