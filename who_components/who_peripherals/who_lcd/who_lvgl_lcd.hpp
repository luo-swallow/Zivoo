#pragma once
#include "esp_lcd_types.h"
#include "bsp/esp-bsp.h"
#if !BSP_CONFIG_NO_GRAPHIC_LIB
#include "lvgl.h"

namespace who {
namespace lcd {

// 全局函数：检查 LVGL 是否已初始化
bool lvgl_is_initialized();

// 全局函数：手动初始化 LVGL（可选）
lv_display_t *lvgl_manual_init();

class WhoLCD {
public:
    WhoLCD(const lvgl_port_cfg_t &lvgl_port_cfg = {4, 10 * 1024, 0, 500, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, 5}) { init(lvgl_port_cfg); }
    ~WhoLCD() { deinit(); }
    void init(const lvgl_port_cfg_t &lvgl_port_cfg);
    void deinit();

private:
    lv_display_t *m_disp;
};
} // namespace lcd
} // namespace who
#endif
