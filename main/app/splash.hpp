#pragma once
#include "lvgl.h"
#include "esp_lvgl_port.h"

namespace who {
namespace ui {

class SplashScreen {
public:
    SplashScreen();
    ~SplashScreen();

    /**
     * @brief 初始化完成显示的启动画面：显示 ai.jpg（来自 SD 卡）和进度条
     */
    bool init();

    /**
     * @brief 逐格推进进度条，每次 +1%，共推进 count 步
     * @param count 推进步数
     * @param delay_ms 每步间隔(ms)
     */
    void step_progress(int count = 1, int delay_ms = 20);

    /**
     * @brief 设置图片下方的提示文本
     * @param text 提示文本内容
     */
    void set_hint_text(const char *text);

    /**
     * @brief 消除启动画面，释放资源
     */
    void dismiss();

private:
    static constexpr int IMAGE_W = 800;
    static constexpr int IMAGE_H = 150;
    static constexpr const char *IMAGE_PATH = "/sdcard/ai.jpg";

    lv_obj_t *m_cont;           // 全屏覆盖层容器
    lv_obj_t *m_canvas;         // 图片 canvas
    lv_obj_t *m_hint_label;     // 图片下方提示文本
    lv_obj_t *m_progress_bar;   // 进度条
    lv_obj_t *m_progress_label; // 百分比标签
    uint8_t *m_image_buffer;    // 解码后的 RGB565 像素数据
    int m_percent = 0;          // 当前进度
};

} // namespace ui
} // namespace who
