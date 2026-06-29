#include "sys.hpp"
#include "who_cam.hpp"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_partition.h"
#include "esp_memory_utils.h"
#include "bsp/esp-bsp.h"
#include "esp_tts.h"
#include "esp_tts_voice_template.h"
#include "cJSON.h"

using namespace who::cam;
using namespace who::frame_cap;

namespace who {
namespace sys {

// 按钮相关静态变量
static button_callback_t s_button_callback = nullptr;
static TaskHandle_t s_button_task_handle = nullptr;
static bool s_button_running = false;
static gpio_num_t s_button_gpio = BUTTON_GPIO_DEFAULT;
static SemaphoreHandle_t s_button_semaphore = nullptr;

static const char *TAG = "ButtonManager";

// GPIO 中断处理函数
static void IRAM_ATTR button_isr_handler(void *arg)
{
    BaseType_t higher_prio_woken = pdFALSE;
    xSemaphoreGiveFromISR(s_button_semaphore, &higher_prio_woken);
    portYIELD_FROM_ISR(higher_prio_woken);
}

// 按钮检测任务
static void button_task(void *arg)
{
    while (s_button_running) {
        // 等待中断信号
        if (xSemaphoreTake(s_button_semaphore, portMAX_DELAY) == pdTRUE) {
            // 软件消抖：延时20ms后再次确认电平
            vTaskDelay(pdMS_TO_TICKS(20));
            if (!s_button_running) break;

            if (gpio_get_level(s_button_gpio) == 1) {
                if (s_button_callback) {
                    s_button_callback();
                }
            }
        }
    }

    s_button_task_handle = nullptr;
    vTaskDelete(nullptr);
}

bool button_init(gpio_num_t gpio_num)
{
    s_button_gpio = gpio_num;

    // 配置GPIO为输入模式（带中断）
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << gpio_num),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,  // 启用下拉，默认低电平
        .intr_type = GPIO_INTR_POSEDGE,         // 上升沿中断
    };

    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GPIO配置失败: %s", esp_err_to_name(ret));
        return false;
    }

    // 创建二值信号量（用于中断同步）
    s_button_semaphore = xSemaphoreCreateBinary();
    if (!s_button_semaphore) {
        ESP_LOGE(TAG, "创建信号量失败");
        return false;
    }

    // 安装 GPIO ISR 服务
    ret = gpio_install_isr_service(ESP_INTR_FLAG_LEVEL1);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "安装ISR服务失败: %s", esp_err_to_name(ret));
        vSemaphoreDelete(s_button_semaphore);
        s_button_semaphore = nullptr;
        return false;
    }

    // 添加 GPIO ISR 处理函数
    ret = gpio_isr_handler_add(gpio_num, button_isr_handler, nullptr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "添加ISR处理函数失败: %s", esp_err_to_name(ret));
        gpio_uninstall_isr_service();
        vSemaphoreDelete(s_button_semaphore);
        s_button_semaphore = nullptr;
        return false;
    }

    return true;
}

void button_set_callback(button_callback_t callback)
{
    s_button_callback = callback;
}

void button_start()
{
    if (s_button_running) {
        ESP_LOGW(TAG, "按钮检测已经在运行");
        return;
    }

    s_button_running = true;

    // 创建按钮检测任务（栈放到 PSRAM，节省内部 DRAM）
    BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(
        button_task,
        "button_task",
        4 * 1024,       // 4KB栈
        nullptr,
        5,              // 优先级5
        &s_button_task_handle,
        0,              // 绑定到Core 0
        MALLOC_CAP_SPIRAM
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "创建按钮检测任务失败");
        s_button_running = false;
    }
}

void button_stop()
{
    if (!s_button_running) {
        return;
    }

    s_button_running = false;

    // 唤醒任务以退出
    if (s_button_semaphore) {
        xSemaphoreGive(s_button_semaphore);
    }

    vTaskDelay(pdMS_TO_TICKS(50));

    gpio_isr_handler_remove(s_button_gpio);
    gpio_uninstall_isr_service();

    if (s_button_semaphore) {
        vSemaphoreDelete(s_button_semaphore);
        s_button_semaphore = nullptr;
    }
}

int button_get_level()
{
    return gpio_get_level(s_button_gpio);
}

// ==================== TTS 语音合成 ====================

static const char *TTS_TAG = "TtsManager";

static esp_tts_handle_t s_tts_handle = nullptr;
static esp_codec_dev_handle_t s_spk_codec = nullptr;
static QueueHandle_t s_tts_queue = nullptr;
static volatile bool s_stop_playback = false;
static bool s_tts_initialized = false;
static constexpr int TTS_MAX_TEXT_LEN = 256;

// 语音播放任务
static void tts_speech_task(void *arg)
{
    char text[TTS_MAX_TEXT_LEN];

    while (1) {
        if (xQueueReceive(s_tts_queue, text, portMAX_DELAY) == pdTRUE) {
            s_stop_playback = false;

            if (esp_tts_parse_chinese(s_tts_handle, text)) {
                int len[1] = {0};

                do {
                    if (s_stop_playback) {
                        break;
                    }
                    short *pcm_data = esp_tts_stream_play(s_tts_handle, len, 3);
                    if (len[0] > 0) {
                        esp_codec_dev_write(s_spk_codec, pcm_data, len[0] * sizeof(int16_t));
                    }
                } while (len[0] > 0 && !s_stop_playback);

                esp_tts_stream_reset(s_tts_handle);
            }
        }
    }
}

bool tts_init()
{
    if (s_tts_initialized) {
        ESP_LOGW(TTS_TAG, "TTS already initialized");
        return true;
    }

    // 0. 配置并禁用扬声器放大器（GPIO53 = PA_EN），防止上电爆音
    gpio_config_t amp_dis = {
        .pin_bit_mask = (1ULL << GPIO_NUM_53),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&amp_dis);
    gpio_set_level(GPIO_NUM_53, 0);

    // 1. 初始化扬声器
    s_spk_codec = bsp_audio_codec_speaker_init();
    if (!s_spk_codec) {
        ESP_LOGE(TTS_TAG, "Failed to init speaker codec");
        return false;
    }

    // BSP 可能已使能放大器，立即再次禁用，等待编解码器稳定
    gpio_set_level(GPIO_NUM_53, 0);

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 1,
        .sample_rate = 16000,
    };
    esp_err_t ret = esp_codec_dev_open(s_spk_codec, &fs);
    if (ret != ESP_OK) {
        ESP_LOGE(TTS_TAG, "Failed to open speaker codec: %s", esp_err_to_name(ret));
        return false;
    }
    esp_codec_dev_set_out_vol(s_spk_codec, 80);

    // 等待编解码器稳定后使能放大器
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(GPIO_NUM_53, 1);

    // 2. 查找 voice_data 分区中的语音模型
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "voice_data");
    if (!part) {
        ESP_LOGE(TTS_TAG, "Voice data partition not found");
        return false;
    }

    const void *voicedata;
    esp_partition_mmap_handle_t mmap_handle;
    ret = esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, &voicedata, &mmap_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TTS_TAG, "Failed to mmap voice data: %s", esp_err_to_name(ret));
        return false;
    }

    // 3. 初始化语音集和 TTS
    esp_tts_voice_t *voice = esp_tts_voice_set_init(&esp_tts_voice_template, (int16_t *)voicedata);
    if (!voice) {
        ESP_LOGE(TTS_TAG, "Failed to init voice set");
        return false;
    }

    s_tts_handle = esp_tts_create(voice);
    if (!s_tts_handle) {
        ESP_LOGE(TTS_TAG, "Failed to create TTS handle");
        return false;
    }

    // 4. 创建播放队列和任务
    s_tts_queue = xQueueCreate(2, TTS_MAX_TEXT_LEN);
    if (!s_tts_queue) {
        ESP_LOGE(TTS_TAG, "Failed to create TTS queue");
        return false;
    }

    BaseType_t task_ret = xTaskCreatePinnedToCoreWithCaps(
        tts_speech_task,
        "tts_task",
        8 * 1024,
        nullptr,
        5,
        nullptr,
        0,
        MALLOC_CAP_SPIRAM);
    if (task_ret != pdPASS) {
        ESP_LOGE(TTS_TAG, "Failed to create TTS task");
        return false;
    }

    s_tts_initialized = true;
    return true;
}

void tts_speak(const char *text)
{
    if (!s_tts_initialized || !s_tts_queue) {
        ESP_LOGW(TTS_TAG, "TTS not initialized");
        return;
    }

    s_stop_playback = true;
    vTaskDelay(pdMS_TO_TICKS(50));
    s_stop_playback = false;

    char text_copy[TTS_MAX_TEXT_LEN];
    strncpy(text_copy, text, TTS_MAX_TEXT_LEN - 1);
    text_copy[TTS_MAX_TEXT_LEN - 1] = '\0';

    xQueueReset(s_tts_queue);
    xQueueSend(s_tts_queue, text_copy, 0);
}

void tts_stop()
{
    if (!s_tts_initialized) return;
    s_stop_playback = true;
    if (s_tts_queue) {
        xQueueReset(s_tts_queue);
    }
    if (s_tts_handle) {
        esp_tts_stream_reset(s_tts_handle);
    }
}

// ==================== 本地知识库（SD卡 JSON） ====================

static const char *KB_TAG = "KnowledgeBase";
static cJSON *s_knowledge_json = nullptr;

bool knowledge_init()
{
    if (s_knowledge_json) {
        return true;
    }

    FILE *f = fopen("/sdcard/knowledge.json", "r");
    if (!f) {
        ESP_LOGE(KB_TAG, "Failed to open /sdcard/knowledge.json");
        return false;
    }

    // 获取文件大小
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0) {
        ESP_LOGE(KB_TAG, "Empty file");
        fclose(f);
        return false;
    }

    // 读取整个文件
    char *json_str = (char*)heap_caps_malloc(fsize + 1, MALLOC_CAP_SPIRAM);
    if (!json_str) {
        ESP_LOGE(KB_TAG, "Failed to allocate memory for knowledge base");
        fclose(f);
        return false;
    }

    size_t read_size = fread(json_str, 1, fsize, f);
    fclose(f);
    json_str[read_size] = '\0';

    s_knowledge_json = cJSON_Parse(json_str);
    free(json_str);

    if (!s_knowledge_json) {
        ESP_LOGE(KB_TAG, "Failed to parse knowledge JSON");
        return false;
    }

    // 检查是否为有效对象
    if (!cJSON_IsObject(s_knowledge_json)) {
        ESP_LOGE(KB_TAG, "Knowledge JSON root is not an object");
        cJSON_Delete(s_knowledge_json);
        s_knowledge_json = nullptr;
        return false;
    }

    int count = cJSON_GetArraySize(s_knowledge_json);
    return true;
}

const char* knowledge_query(const char* key)
{
    if (!s_knowledge_json || !key) {
        return nullptr;
    }

    cJSON *item = cJSON_GetObjectItem(s_knowledge_json, key);
    if (item && cJSON_IsString(item) && cJSON_GetStringValue(item)) {
        return cJSON_GetStringValue(item);
    }

    return nullptr;
}

void knowledge_deinit()
{
    if (s_knowledge_json) {
        cJSON_Delete(s_knowledge_json);
        s_knowledge_json = nullptr;
        ESP_LOGI(KB_TAG, "Knowledge base unloaded");
    }
}

} // namespace sys
} // namespace who

// 帧捕获流水线创建函数
who::frame_cap::WhoFrameCap *who::sys::get_lcd_mipi_csi_ppa_frame_cap_pipeline(
    who::frame_cap::WhoFrameCapNode **lcd_disp_frame_cap_node)
{
    auto cam = new WhoP4Cam(V4L2_PIX_FMT_RGB565, 9);
    auto frame_cap = new WhoFrameCap();

    // 1. 添加帧获取节点（原始帧）
    frame_cap->add_node<WhoFetchNode>("FrameCapFetch", cam);

    // 2. 添加PPA硬件缩放节点 - 缩放到500x500用于LCD显示
    frame_cap->add_node<WhoPPAResizeNode>(
        "FrameCapPPALCDResize",
        LCD_DISP_W,
        LCD_DISP_H,
        dl::image::DL_IMAGE_PIX_TYPE_RGB565LE,
        MODEL_TIME);

    // 3. 返回500x500缩放节点给LCD显示
    *lcd_disp_frame_cap_node = frame_cap->get_node("FrameCapPPALCDResize");

    // 4. 继续缩放到320x320用于模型推理（从500x500再缩放）
    frame_cap->add_node<WhoPPAResizeNode>(
        "FrameCapPPAModelResize",
        MODEL_INPUT_W,
        MODEL_INPUT_H,
        dl::image::DL_IMAGE_PIX_TYPE_RGB565LE,
        MODEL_TIME);

    return frame_cap;
}