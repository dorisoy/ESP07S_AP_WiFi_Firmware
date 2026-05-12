cmake_minimum_required(VERSION 3.5)

# ESP8266 RTOS SDK 项目根 CMakeLists.txt
# 使用方法：
#   export IDF_PATH=/path/to/ESP8266_RTOS_SDK
#   idf.py build

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp07s_wifi_manager)
