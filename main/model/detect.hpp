#pragma once

#include "dl_detect_base.hpp"
#include "dl_detect_yolo11_postprocessor.hpp"
#include "dl_image_preprocessor.hpp"
#include "dl_model_base.hpp"
#include <filesystem>

// SD卡模型路径
#define SD_MODEL_PATH "/sdcard/models/p4"

/**
 * @brief YOLO11检测模型 - 从SD卡加载
 */
class YOLO11Detect : public dl::detect::DetectImpl {
public:
    static constexpr float default_score_thr = 0.75f;
    static constexpr float default_nms_thr = 0.7f;

    YOLO11Detect(const char *model_name, float score_thr = default_score_thr, float nms_thr = default_nms_thr);
};

/**
 * @brief 获取检测模型 - 从SD卡加载指定模型
 * @param model_name 模型文件名 (位于 SD卡 /models/p4/)
 */
dl::detect::Detect *get_detect_model(const char *model_name = "coco_detect_yolo11n_320_s8_v3.espdl");