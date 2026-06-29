#include "speech_recognizer.hpp"
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"

// ESP-SR 相关头文件
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "esp_codec_dev.h"
#include "sys.hpp"

static const char *TAG = "SpeechRecognizer";

namespace who {
namespace speech {

SpeechRecognizer* SpeechRecognizer::get_instance()
{
    static SpeechRecognizer instance;
    return &instance;
}

SpeechRecognizer::SpeechRecognizer() :
    m_initialized(false),
    m_running(false),
    m_command_cb(nullptr),
    m_afe_handle(nullptr),
    m_afe_data(nullptr),
    m_mic_codec(nullptr),
    m_sr_models(nullptr),
    m_task_flag(0)
{
}

SpeechRecognizer::~SpeechRecognizer()
{
    stop();
}
// 语音命令枚举 → 中文描述字符串，用于日志和语音反馈
const char* SpeechRecognizer::get_command_name(VoiceCommand cmd)
{
    switch (cmd) {
        case VoiceCommand::CMD_PAI_ZHAO:       return "帮我拍照";
        case VoiceCommand::CMD_SHI_BIE:        return "帮我识别";
        case VoiceCommand::CMD_SHAN_CHU:       return "帮我删除照片";
        case VoiceCommand::CMD_XIA_YI_ZHANG:   return "下一张";
        case VoiceCommand::CMD_SHANG_YI_ZHANG: return "上一张";
        case VoiceCommand::CMD_FA_SONG:        return "发送";
        case VoiceCommand::CMD_LIAN_JIE_WIFI:  return "加载网络";
        case VoiceCommand::CMD_GUAN_BI_SHENG_YIN: return "关闭声音";
        case VoiceCommand::CMD_DA_KAI_SHENG_YIN:  return "打开声音";
        case VoiceCommand::CMD_PAI_ZHAO_SHI_BIE:  return "拍照识别";
        case VoiceCommand::CMD_DA_KAI_ASSIST:     return "打开助手模式";
        case VoiceCommand::CMD_GUAN_BI_ASSIST:    return "关闭助手模式";
        default:                               return "未知命令";
    }
}

bool SpeechRecognizer::init()
{
    if (m_initialized) {
        ESP_LOGW(TAG, "Speech recognizer already initialized");
        return true;
    }

    // 初始化 I2C
    esp_err_t ret = bsp_i2c_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to initialize I2C: %s", esp_err_to_name(ret));
        return false;
    }

    // 配置 I2S
    i2s_std_config_t i2s_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_SCLK,
            .ws = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT,
            .din = BSP_I2S_DSIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ret = bsp_audio_init(&i2s_config);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to initialize audio: %s", esp_err_to_name(ret));
        return false;
    }

    // 初始化麦克风
    m_mic_codec = bsp_audio_codec_microphone_init();
    if (m_mic_codec == nullptr) {
        ESP_LOGE(TAG, "Failed to initialize microphone codec");
        return false;
    }

    // 打开麦克风
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = 0,
        .sample_rate = 16000,
        .mclk_multiple = 0,
    };

    ret = esp_codec_dev_open((esp_codec_dev_handle_t)m_mic_codec, &fs);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to open microphone codec");
        return false;
    }

    // 初始化语音模型（从 SD 卡加载）
    m_sr_models = esp_srmodel_init("/sdcard/models/sr");
    if (m_sr_models == nullptr) {
        ESP_LOGE(TAG, "Failed to init speech models");
        return false;
    }

    // 检查 WakeNet 模型
    char *wn_name = esp_srmodel_filter((srmodel_list_t*)m_sr_models, ESP_WN_PREFIX, NULL);
    if (!wn_name) {
        ESP_LOGE(TAG, "No WakeNet model found!");
        return false;
    }

    // 检查 MultiNet 模型
    char *mn_name = esp_srmodel_filter((srmodel_list_t*)m_sr_models, ESP_MN_PREFIX, ESP_MN_CHINESE);
    if (!mn_name) {
        ESP_LOGE(TAG, "No MultiNet model found!");
        return false;
    }

    // 配置 AFE
    afe_config_t *afe_config = afe_config_init("M", (srmodel_list_t*)m_sr_models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (afe_config == nullptr) {
        ESP_LOGE(TAG, "Failed to init AFE config");
        return false;
    }

    // 增大 ringbuffer 容量，防止 feed 快于 fetch 时丢失数据
    afe_config->afe_ringbuf_size = 150;

    m_afe_handle = (void*)esp_afe_handle_from_config(afe_config);
    if (m_afe_handle == nullptr) {
        ESP_LOGE(TAG, "Failed to get AFE handle");
        afe_config_free(afe_config);
        return false;
    }

    m_afe_data = ((esp_afe_sr_iface_t*)m_afe_handle)->create_from_config(afe_config);
    if (m_afe_data == nullptr) {
        ESP_LOGE(TAG, "Failed to create AFE data");
        afe_config_free(afe_config);
        return false;
    }

    afe_config_free(afe_config);

    m_initialized = true;
    return true;
}

void SpeechRecognizer::start()
{
    if (!m_initialized) {
        ESP_LOGE(TAG, "Speech recognizer not initialized");
        return;
    }

    if (m_running) {
        ESP_LOGW(TAG, "Speech recognizer already running");
        return;
    }

    m_task_flag = 1;
    m_running = true;

    TaskHandle_t detect_task_handle = NULL;
    TaskHandle_t feed_task_handle = NULL;

    // 创建检测任务
    BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(detect_task, "sr_detect_task", 20 * 1024, this, 6, &detect_task_handle, 1, MALLOC_CAP_SPIRAM);
    if (ret != pdPASS || detect_task_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create detect task! ret=%d", ret);
        m_task_flag = 0;
        m_running = false;
        return;
    }

    // 等待检测任务初始化完成
    vTaskDelay(pdMS_TO_TICKS(500));

    // 创建音频输入任务 (Core 1)
    ret = xTaskCreatePinnedToCoreWithCaps(feed_task, "sr_feed_task", 10 * 1024, this, 5, &feed_task_handle, 1, MALLOC_CAP_SPIRAM);
    if (ret != pdPASS || feed_task_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create feed task! ret=%d", ret);
        m_task_flag = 0;
        m_running = false;
        return;
    }
}

void SpeechRecognizer::stop()
{
    if (!m_running) {
        return;
    }

    m_task_flag = 0;
    m_running = false;

    vTaskDelay(pdMS_TO_TICKS(100));
}

void SpeechRecognizer::execute_command(VoiceCommand cmd, int64_t sr_time_ms)
{
    if (m_command_cb) {
        m_command_cb(cmd, sr_time_ms);
    }
}

void SpeechRecognizer::feed_task(void* arg)
{
    SpeechRecognizer* self = static_cast<SpeechRecognizer*>(arg);
    esp_afe_sr_iface_t* afe_handle = (esp_afe_sr_iface_t*)self->m_afe_handle;
    esp_afe_sr_data_t* afe_data = (esp_afe_sr_data_t*)self->m_afe_data;
    esp_codec_dev_handle_t mic_codec = (esp_codec_dev_handle_t)self->m_mic_codec;

    int audio_chunksize = afe_handle->get_feed_chunksize(afe_data);
    int feed_channel = afe_handle->get_feed_channel_num(afe_data);
    int buffer_size = audio_chunksize * sizeof(int16_t) * feed_channel;
    int16_t *i2s_buff = (int16_t *)heap_caps_malloc(buffer_size, MALLOC_CAP_8BIT);

    while (self->m_task_flag) {
        int ret = esp_codec_dev_read(mic_codec, i2s_buff, buffer_size);
        if (ret == 0) {
            afe_handle->feed(afe_data, i2s_buff);
        } else {
            ESP_LOGW(TAG, "Failed to read audio data: %d", ret);
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    if (i2s_buff) {
        free(i2s_buff);
    }

    vTaskDelete(NULL);
}

void SpeechRecognizer::detect_task(void* arg)
{
    SpeechRecognizer* self = static_cast<SpeechRecognizer*>(arg);
    esp_afe_sr_iface_t* afe_handle = (esp_afe_sr_iface_t*)self->m_afe_handle;
    esp_afe_sr_data_t* afe_data = (esp_afe_sr_data_t*)self->m_afe_data;
    srmodel_list_t* sr_models = (srmodel_list_t*)self->m_sr_models;

    // 设置唤醒阈值
    afe_handle->set_wakenet_threshold(afe_data, 1, 0.5);

    // 获取 MultiNet 模型
    char *mn_name = esp_srmodel_filter(sr_models, ESP_MN_PREFIX, ESP_MN_CHINESE);
    if (mn_name == NULL) {
        ESP_LOGE(TAG, "Failed to get MultiNet model");
        vTaskDelete(NULL);
        return;
    }

    esp_mn_iface_t *multinet = esp_mn_handle_from_name(mn_name);
    model_iface_data_t *model_data = multinet->create(mn_name, 6000);

    // 注册语音命令
    esp_mn_commands_clear();
    esp_mn_commands_add((int)VoiceCommand::CMD_SHI_BIE, "bang wo shi bie");          // 帮我识别
    esp_mn_commands_add((int)VoiceCommand::CMD_SHAN_CHU, "bang wo shan chu zhao pian"); // 帮我删除照片
    esp_mn_commands_add((int)VoiceCommand::CMD_XIA_YI_ZHANG, "xia yi zhang");           // 下一张
    esp_mn_commands_add((int)VoiceCommand::CMD_SHANG_YI_ZHANG, "shang yi zhang");       // 上一张
    esp_mn_commands_add((int)VoiceCommand::CMD_FA_SONG, "fa song");                     // 发送
    esp_mn_commands_add((int)VoiceCommand::CMD_LIAN_JIE_WIFI, "jia zai wang luo");  // 加载网络
    esp_mn_commands_add((int)VoiceCommand::CMD_GUAN_BI_SHENG_YIN, "guan bi sheng yin");  // 关闭声音
    esp_mn_commands_add((int)VoiceCommand::CMD_DA_KAI_SHENG_YIN, "da kai sheng yin");  // 打开声音
    esp_mn_commands_add((int)VoiceCommand::CMD_PAI_ZHAO_SHI_BIE, "bang wo pai zhao");  // 拍照识别
    esp_mn_commands_add((int)VoiceCommand::CMD_DA_KAI_ASSIST, "da kai zhu shou mo shi");  // 打开助手模式
    esp_mn_commands_add((int)VoiceCommand::CMD_GUAN_BI_ASSIST, "guan bi zhu shou mo shi");  // 关闭助手模式
    esp_mn_commands_update();

    bool is_woken = false;
    int64_t t_detect_start = esp_timer_get_time();

    while (self->m_task_flag) {
        afe_fetch_result_t *res = afe_handle->fetch(afe_data);
        if (!res || res->ret_value == ESP_FAIL) {
            ESP_LOGE(TAG, "AFE fetch error!");
            break;
        }

        // 检测唤醒词
        if (res->wakeup_state == WAKENET_DETECTED) {
            ESP_LOGI(TAG, "[PERF] 语音唤醒词检测成功");
            who::sys::tts_speak("我在");
            is_woken = true;
        }

        if (!is_woken) {
            continue;
        }

        // 检测命令
        int64_t t_cmd_start = esp_timer_get_time();
        esp_mn_state_t mn_state = multinet->detect(model_data, res->data);

        if (mn_state == ESP_MN_STATE_DETECTED) {
            esp_mn_results_t *result = multinet->get_results(model_data);
            int cmd_id = result->command_id[0];
            int64_t t_cmd_end = esp_timer_get_time();
            int64_t sr_time_ms = (t_cmd_end - t_cmd_start) / 1000;
            ESP_LOGI(TAG, "[PERF] 语音命令识别: %lld ms, 命令=%d(%s)",
                     sr_time_ms, cmd_id,
                     get_command_name((VoiceCommand)cmd_id));

            // 执行命令
            self->execute_command((VoiceCommand)cmd_id, sr_time_ms);

            // 保持唤醒状态，可连续执行命令
        }
        // 不处理超时，唤醒后一直保持唤醒状态
    }

    multinet->destroy(model_data);
    vTaskDelete(NULL);
}

} // namespace speech
} // namespace who
