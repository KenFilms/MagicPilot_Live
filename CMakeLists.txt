cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Release builds pass the version (tools/build_installer.py); otherwise ESP-IDF uses git describe
if(DEFINED ENV{MAGICPILOT_VERSION})
    set(PROJECT_VER "$ENV{MAGICPILOT_VERSION}")
endif()
project(magicpilot_live)
