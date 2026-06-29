idf_component_register(SRCS "main.cpp" "sys.cpp" "model/detect.cpp" "model/image_process.cpp" "model/coco_classes.cpp" "app/index.cpp" "app/index_buttons.cpp" "app/splash.cpp" "app/wifi_manager.cpp" "app/llm_client.cpp" "app/speech_recognizer.cpp" "app/lv_font_alibabapuhuiti_light_16.c"
                    INCLUDE_DIRS "." "model" "app"
                    REQUIRES esp-dl who_cam who_frame_cap who_detect who_task who_frame_lcd_disp who_app_common who_detect_app esp_wifi esp_wifi_remote esp_hosted nvs_flash esp_netif esp_http_client json mbedtls esp-sr)

# 启用 ESP32-P4 RVV SIMD 加速（ESP-DL 向量优化内核）
idf_component_get_property(esp_dl espressif__esp-dl COMPONENT_LIB)
target_compile_options(${esp_dl} PRIVATE "-DCONFIG_ESP32P4_BOOST")

# 烧录 TTS 语音模型到 voice_data 分区
idf_component_get_property(esp_sr_dir espressif__esp-sr COMPONENT_DIR)
set(voice_data_image ${esp_sr_dir}/esp-tts/esp_tts_chinese/esp_tts_voice_data_xiaoxin.dat)
partition_table_get_partition_info(offset "--partition-name voice_data" "offset")
if(offset)
    esptool_py_flash_to_partition(flash "voice_data" "${voice_data_image}")
endif()
