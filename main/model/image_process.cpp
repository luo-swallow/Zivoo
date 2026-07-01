#include "image_process.hpp"
#include <sys/stat.h>
#include <dirent.h>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include "esp_timer.h"

static const char *TAG = "ImageManager";

namespace who {
namespace image {

ImageManager::ImageManager() :
    m_image_list(),
    m_current_index(-1),
    m_capture_counter(0)
{
}

ImageManager::~ImageManager()
{
}

void ImageManager::init()
{
    ensure_folder_exists(CAPTURE_DIR);
    scan_capture_folder();
}

void ImageManager::ensure_folder_exists(const char *path)
{
    struct stat st;
    if (stat(path, &st) == -1) {
        if (mkdir(path, 0777) == -1) {
            ESP_LOGE(TAG, "Failed to create directory: %s", path);
        }
    }
}

void ImageManager::scan_capture_folder()
{
    m_image_list.clear();
    m_current_index = -1;
    m_capture_counter = 0;

    DIR *dir = opendir(CAPTURE_DIR);
    if (!dir) {
        ESP_LOGW(TAG, "Cannot open captures directory");
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name.size() > 4) {
            std::string ext = name.substr(name.size() - 4);
            if (ext == ".jpg" || ext == ".JPG") {
                std::string full_path = std::string(CAPTURE_DIR) + "/" + name;
                m_image_list.push_back(full_path);

                if (name.substr(0, 4) == "cap_" && name.size() >= 8) {
                    std::string num_str = name.substr(4, 3);
                    int num = atoi(num_str.c_str());
                    if (num > m_capture_counter) {
                        m_capture_counter = num;
                    }
                }
            }
        }
    }
    closedir(dir);

    std::sort(m_image_list.begin(), m_image_list.end());

    if (!m_image_list.empty()) {
        m_current_index = 0;
    }

}

std::vector<std::string>& ImageManager::get_image_list()
{
    return m_image_list;
}

int ImageManager::get_image_count()
{
    return m_image_list.size();
}

int ImageManager::get_current_index()
{
    return m_current_index;
}

bool ImageManager::prev_image()
{
    if (m_image_list.empty()) {
        return false;
    }

    if (m_current_index > 0) {
        m_current_index--;
        return true;
    } else {
        m_current_index = m_image_list.size() - 1;
        return true;
    }
}

bool ImageManager::next_image()
{
    if (m_image_list.empty()) {
        return false;
    }

    if (m_current_index < (int)m_image_list.size() - 1) {
        m_current_index++;
        return true;
    } else {
        m_current_index = 0;
        return true;
    }
}

bool ImageManager::delete_current_image()
{
    if (m_image_list.empty() || m_current_index < 0 || m_current_index >= (int)m_image_list.size()) {
        ESP_LOGW(TAG, "No image to delete");
        return false;
    }

    std::string image_path = m_image_list[m_current_index];
    if (remove(image_path.c_str()) != 0) {
        ESP_LOGE(TAG, "Failed to delete image file: %s", image_path.c_str());
        return false;
    }

    std::string result_path = get_result_path(image_path);
    remove(result_path.c_str());

    m_image_list.erase(m_image_list.begin() + m_current_index);

    if (m_image_list.empty()) {
        m_current_index = -1;
        return true;
    } else if (m_current_index >= (int)m_image_list.size()) {
        m_current_index = m_image_list.size() - 1;
    }

    return true;
}

std::string ImageManager::get_current_image_path()
{
    if (m_current_index >= 0 && m_current_index < (int)m_image_list.size()) {
        return m_image_list[m_current_index];
    }
    return "";
}

std::string ImageManager::get_current_image_name()
{
    std::string path = get_current_image_path();
    if (path.empty()) {
        return "";
    }

    size_t pos = path.find_last_of('/');
    if (pos != std::string::npos) {
        return path.substr(pos + 1);
    }
    return path;
}

std::string ImageManager::generate_timestamp_filename()
{
    m_capture_counter++;
    char buf[32];
    snprintf(buf, sizeof(buf), "cap_%03d.jpg", m_capture_counter);
    return std::string(buf);
}

std::string ImageManager::save_capture_frame(who::cam::cam_fb_t *fb)
{
    if (!fb || !fb->buf) {
        ESP_LOGE(TAG, "Invalid frame buffer");
        return "";
    }

    std::string filename = generate_timestamp_filename();
    std::string filepath = std::string(CAPTURE_DIR) + "/" + filename;

    dl::image::img_t img;
    img.data = fb->buf;
    img.width = fb->width;
    img.height = fb->height;

    if (fb->format == who::cam::cam_fb_fmt_t::CAM_FB_FMT_RGB565) {
        img.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565LE;
    } else if (fb->format == who::cam::cam_fb_fmt_t::CAM_FB_FMT_RGB888) {
        img.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB888;
    } else {
        ESP_LOGE(TAG, "Unsupported frame format");
        return "";
    }

    int64_t t_jpeg_start = esp_timer_get_time();
    dl::image::jpeg_img_t jpeg_img = {nullptr, 0};

    if (!jpeg_img.data) {
        // ov5647的RGB565软件编码亮度异常，手动扩展为RGB888再编码
        if (img.pix_type == dl::image::DL_IMAGE_PIX_TYPE_RGB565LE) {
            size_t px = (size_t)img.width * img.height;
            uint8_t *rgb888 = (uint8_t *)heap_caps_malloc(px * 3, MALLOC_CAP_SPIRAM);
            if (rgb888) {
                uint8_t *s = (uint8_t *)img.data;
                for (size_t i = 0; i < px; i++) {
                    uint16_t p = s[0] | ((uint16_t)s[1] << 8);
                    uint8_t r5 = (p >> 11) & 0x1f, g6 = (p >> 5) & 0x3f, b5 = p & 0x1f;
                    rgb888[i * 3]     = (r5 << 3) | (r5 >> 2);  // 5→8 bit
                    rgb888[i * 3 + 1] = (g6 << 2) | (g6 >> 4);  // 6→8 bit
                    rgb888[i * 3 + 2] = (b5 << 3) | (b5 >> 2);  // 5→8 bit
                    s += 2;
                }
                dl::image::img_t img_rgb(rgb888, img.width, img.height, dl::image::DL_IMAGE_PIX_TYPE_RGB888);
                jpeg_img = dl::image::sw_encode_jpeg(img_rgb, 85);
                heap_caps_free(rgb888);
            }
        }
        if (!jpeg_img.data) {
            jpeg_img = dl::image::sw_encode_jpeg(img, 85);
        }
    }
    int64_t t_jpeg_end = esp_timer_get_time();

    if (!jpeg_img.data) {
        ESP_LOGE(TAG, "Failed to encode JPEG");
        return "";
    }
    ESP_LOGI(TAG, "[PERF] JPEG编码: %lld ms (质量85, 大小%d bytes)",
             (t_jpeg_end - t_jpeg_start) / 1000, jpeg_img.data_len);

    esp_err_t ret = dl::image::write_jpeg(jpeg_img, filepath.c_str());
    heap_caps_free(jpeg_img.data);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write JPEG file");
        return "";
    }

    m_image_list.push_back(filepath);
    m_current_index = m_image_list.size() - 1;

    return filepath;
}

std::string ImageManager::get_result_path(const std::string &capture_path)
{
    size_t pos = capture_path.find_last_of('/');
    std::string filename;
    if (pos != std::string::npos) {
        filename = capture_path.substr(pos + 1);
    } else {
        filename = capture_path;
    }

    if (filename.substr(0, 4) == "cap_") {
        filename = "res_" + filename.substr(4);
    } else {
        filename = "res_" + filename;
    }

    return std::string(RESULT_DIR) + "/" + filename;
}

std::string ImageManager::get_result_path_by_categories(const std::list<dl::detect::result_t> &results)
{
    if (results.empty()) {
        // 没有检测结果，返回空字符串表示不保存
        return "";
    }

    // 收集所有唯一的 category
    std::vector<int> categories;
    for (const auto &res : results) {
        if (std::find(categories.begin(), categories.end(), res.category) == categories.end()) {
            categories.push_back(res.category);
        }
    }

    // 排序 category 使文件名一致
    std::sort(categories.begin(), categories.end());

    // 用 _ 连接所有 category
    std::string filename;
    for (size_t i = 0; i < categories.size(); i++) {
        filename += std::to_string(categories[i]);
        if (i < categories.size() - 1) {
            filename += "_";
        }
    }
    filename += ".jpg";

    return std::string(RESULT_DIR) + "/" + filename;
}

void ImageManager::clear_results_folder()
{
    DIR *dir = opendir(RESULT_DIR);
    if (!dir) {
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name != "." && name != "..") {
            std::string filepath = std::string(RESULT_DIR) + "/" + name;
            remove(filepath.c_str());
        }
    }
    closedir(dir);
}

} // namespace image
} // namespace who