#include "detect.hpp"
#include "esp_log.h"

static const char *TAG = "YOLO11Detect";

YOLO11Detect::YOLO11Detect(const char *model_name, float score_thr, float nms_thr)
{
    // 构建SD卡完整路径
    auto sd_path = std::filesystem::path(SD_MODEL_PATH) / model_name;

    // 从SD卡加载模型
    m_model = new dl::Model(sd_path.c_str(), fbs::MODEL_LOCATION_IN_SDCARD);
    m_model->minimize();

    // 图像预处理: mean=0, std=255, RGB格式
    m_image_preprocessor = new dl::image::ImagePreprocessor(m_model, {0, 0, 0}, {255, 255, 255});

    // YOLO11后处理: 3个检测头，stride为8, 16, 32
    m_postprocessor = new dl::detect::yolo11PostProcessor(
        m_model, m_image_preprocessor, score_thr, nms_thr, 100,
        {{8, 8, 0, 0}, {16, 16, 0, 0}, {32, 32, 0, 0}});

}

dl::detect::Detect *get_detect_model(const char *model_name)
{
    return new YOLO11Detect(model_name);
}