#pragma once

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_timer.h"
#include <functional>

namespace who {
namespace wifi {

// WiFi 连接状态
enum class WifiState {
    DISCONNECTED,
    CONNECTING,
    CONNECTED,
    FAILED
};

// WiFi 状态回调函数类型
using WifiStatusCallback = std::function<void(WifiState state)>;

class WifiManager {
public:
    static WifiManager* get_instance();

    // 初始化 WiFi
    bool init();

    // 连接 WiFi（自动重试，默认 5 次）
    bool connect(const char* ssid, const char* password, int max_retries = 5);

    // 断开 WiFi
    void disconnect();

    // 获取当前 WiFi 状态
    WifiState get_state() const { return m_state; }

    // 检查 WiFi 是否已连接
    bool is_connected() const { return m_state == WifiState::CONNECTED; }

    // 设置 WiFi 状态变化回调
    void set_status_callback(WifiStatusCallback callback);

    // 切换 WiFi 连接（已连则断开，未连则连接）
    void toggle_connection();

private:
    WifiManager();
    ~WifiManager();

    // 删除拷贝构造函数和赋值运算符
    WifiManager(const WifiManager&) = delete;
    WifiManager& operator=(const WifiManager&) = delete;

    // WiFi 事件处理器
    static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                   int32_t event_id, void* event_data);

    // IP 事件处理器
    static void ip_event_handler(void* arg, esp_event_base_t event_base,
                                 int32_t event_id, void* event_data);

    // 更新状态并通知回调
    void update_state(WifiState new_state);

    WifiState m_state;
    bool m_initialized;
    bool m_connecting;
    WifiStatusCallback m_status_callback;

    // 重试相关
    int m_retry_count;
    int m_max_retries;
    char m_ssid[16];
    char m_password[16];

    // WiFi 凭据
    static constexpr const char* TAG = "WifiManager";
};

} // namespace wifi
} // namespace who
