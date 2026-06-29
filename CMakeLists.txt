# CMakeLists.txt for the my-ai project
cmake_minimum_required(VERSION 3.16)

# Include all components (including nested subcomponents)
set(EXTRA_COMPONENT_DIRS
    "who_components/who_detect"
    "who_components/who_frame_cap"
    "who_components/who_frame_lcd_disp"
    "who_components/who_qrcode"
    "who_components/who_recognition"
    "who_components/who_task"
    "who_components/who_app/who_detect_app"
    "who_components/who_app/who_qrcode_app"
    "who_components/who_app/who_recognition_app"
    "who_components/who_app/who_app_common"
    "who_components/who_peripherals/who_cam"
    "who_components/who_peripherals/who_lcd"
    "who_components/who_peripherals/who_spiflash_fatfs"
    "who_components/who_peripherals/who_usb"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(my-ai)
