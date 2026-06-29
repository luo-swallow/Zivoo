#include "wifi_manager.hpp"
#include "esp_wifi.h"
#include "esp_wifi_remote.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_hosted.h"
#include <cstring>

// WiFi 配置 
#define WIFI_SSID "max"
#define WIFI_PASSWORD "88888888"

namespace who {
namespace wifi {

WifiManager* WifiManager::get_instance()
{
    static WifiManager instance;
    return &instance;
}

WifiManager::WifiManager() :
    m_state(WifiState::DISCONNECTED),
    m_initialized(false),
    m_connecting(false),
    m_status_callback(nullptr),
    m_retry_count(0),
    m_max_retries(0),
    m_ssid{0},
    m_password{0}
{
}

WifiManager::~WifiManager()
{
}

bool WifiManager::init()
{
    if (m_initialized) {
        ESP_LOGW(TAG, "WiFi already initialized");
        return true;
    }

    // 初始化 NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize NVS: %s", esp_err_to_name(ret));
        return false;
    }

    // 初始化 TCP/IP 协议栈
    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to initialize netif: %s", esp_err_to_name(ret));
        return false;
    }

    // 创建默认事件循环
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to create event loop: %s", esp_err_to_name(ret));
        return false;
    }

    // 创建默认 WiFi STA 网络接口
    esp_netif_create_default_wifi_sta();

    // 初始化 ESP Hosted（通过 SDIO 连接 ESP32-C6 WiFi 协处理器）
    ret = esp_hosted_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize ESP Hosted: %s", esp_err_to_name(ret));
        return false;
    }

    // 使用默认配置初始化 WiFi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize WiFi: %s", esp_err_to_name(ret));
        return false;
    }

    // 注册事件处理器
    esp_event_handler_instance_t instance_wifi;
    esp_event_handler_instance_t instance_ip;

    ret = esp_event_handler_instance_register(WIFI_EVENT,
                                               ESP_EVENT_ANY_ID,
                                               &wifi_event_handler,
                                               this,
                                               &instance_wifi);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register WiFi event handler: %s", esp_err_to_name(ret));
        return false;
    }

    ret = esp_event_handler_instance_register(IP_EVENT,
                                               IP_EVENT_STA_GOT_IP,
                                               &ip_event_handler,
                                               this,
                                               &instance_ip);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register IP event handler: %s", esp_err_to_name(ret));
        return false;
    }

    // 设置 WiFi 模式为 Station
    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set WiFi mode: %s", esp_err_to_name(ret));
        return false;
    }

    // 启动 WiFi
    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WiFi: %s", esp_err_to_name(ret));
        return false;
    }

    m_initialized = true;
    return true;
}

bool WifiManager::connect(const char* ssid, const char* password, int max_retries)
{
    if (!m_initialized) {
        ESP_LOGE(TAG, "WiFi not initialized");
        return false;
    }

    if (m_connecting) {
        ESP_LOGW(TAG, "Already connecting to WiFi");
        return false;
    }

    // 保存凭据用于重试
    strlcpy(m_ssid, ssid, sizeof(m_ssid));
    strlcpy(m_password, password, sizeof(m_password));
    m_max_retries = max_retries;
    m_retry_count = 0;

    // 配置 WiFi
    wifi_config_t wifi_config = {};
    strlcpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char*)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set WiFi config: %s", esp_err_to_name(ret));
        return false;
    }

    m_connecting = true;
    update_state(WifiState::CONNECTING);

    ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WiFi connection: %s", esp_err_to_name(ret));
        m_connecting = false;
        update_state(WifiState::FAILED);
        return false;
    }

    return true;
}

void WifiManager::disconnect()
{
    if (!m_initialized) {
        return;
    }

    if (m_state == WifiState::DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi already disconnected");
        return;
    }

    m_connecting = false;
    m_max_retries = 0;  // 禁用自动重连
    m_retry_count = 0;  // 重置重试计数
    esp_wifi_disconnect();
    update_state(WifiState::DISCONNECTED);
}

void WifiManager::set_status_callback(WifiStatusCallback callback)
{
    m_status_callback = callback;
}

void WifiManager::toggle_connection()
{
    if (m_state == WifiState::CONNECTED) {
        disconnect();
    } else if (m_state == WifiState::DISCONNECTED || m_state == WifiState::FAILED) {
        connect(WIFI_SSID, WIFI_PASSWORD);
    }
}

void WifiManager::update_state(WifiState new_state)
{
    if (m_state != new_state) {
        m_state = new_state;

        if (m_status_callback) {
            m_status_callback(new_state);
        }
    }
}

void WifiManager::wifi_event_handler(void* arg, esp_event_base_t event_base,
                                     int32_t event_id, void* event_data)
{
    WifiManager* manager = static_cast<WifiManager*>(arg);

    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
            case WIFI_EVENT_STA_CONNECTED:
                break;

            case WIFI_EVENT_STA_DISCONNECTED:
                manager->m_connecting = false;

                if (manager->m_state == WifiState::DISCONNECTED) {
                    break;
                }

                // 检查是否需要重试连接
                if (manager->m_max_retries > 0 && manager->m_retry_count < manager->m_max_retries) {
                    manager->m_retry_count++;
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    manager->m_connecting = true;
                    esp_wifi_connect();
                } else {
                    manager->update_state(WifiState::FAILED);
                }
                break;

            default:
                break;
        }
    }
}

void WifiManager::ip_event_handler(void* arg, esp_event_base_t event_base,
                                   int32_t event_id, void* event_data)
{
    WifiManager* manager = static_cast<WifiManager*>(arg);

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
        manager->m_connecting = false;
        manager->m_retry_count = 0;  // 连接成功，重置重试计数
        manager->update_state(WifiState::CONNECTED);
    }
}

} // namespace wifi
} // namespace who
