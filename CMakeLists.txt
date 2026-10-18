cmake_minimum_required(VERSION 3.21)
project(OpenRGBWirelessStrimerPlugin VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# Path to an OpenRGB source checkout matching the installed OpenRGB build
# (plugin API version 5, OpenRGB 1.1).
set(OPENRGB_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/OpenRGB" CACHE PATH "OpenRGB source tree")

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

set(LIBUSB_DIR "${OPENRGB_SOURCE_DIR}/dependencies/libusb-1.0.27")

add_library(OpenRGBWirelessStrimerPlugin SHARED
    src/LianLiWirelessProtocol.cpp
    src/LianLiWirelessProtocol.h
    src/WirelessStrimerManager.cpp
    src/WirelessStrimerManager.h
    src/WirelessStrimerController.cpp
    src/WirelessStrimerController.h
    src/WirelessStrimerWidget.cpp
    src/WirelessStrimerWidget.h
    src/OpenRGBWirelessStrimerPlugin.cpp
    src/OpenRGBWirelessStrimerPlugin.h
)

target_include_directories(OpenRGBWirelessStrimerPlugin PRIVATE
    src
    "${OPENRGB_SOURCE_DIR}"
    "${OPENRGB_SOURCE_DIR}/RGBController"
    "${OPENRGB_SOURCE_DIR}/dependencies/json"
    "${LIBUSB_DIR}/include"
)

target_compile_definitions(OpenRGBWirelessStrimerPlugin PRIVATE
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    PLUGIN_VERSION_STRING="${PROJECT_VERSION}"
)

if(MSVC)
    target_compile_options(OpenRGBWirelessStrimerPlugin PRIVATE /W3 /utf-8 /Zc:__cplusplus)
    # Link libusb statically so the plugin does not depend on DLL search order.
    target_link_libraries(OpenRGBWirelessStrimerPlugin PRIVATE "${LIBUSB_DIR}/VS2022/MS64/static/libusb-1.0.lib")
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBUSB REQUIRED libusb-1.0)
    target_include_directories(OpenRGBWirelessStrimerPlugin PRIVATE ${LIBUSB_INCLUDE_DIRS})
    target_link_libraries(OpenRGBWirelessStrimerPlugin PRIVATE ${LIBUSB_LIBRARIES})
endif()

target_link_libraries(OpenRGBWirelessStrimerPlugin PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)

# Standalone protocol test tool (not loaded by OpenRGB)
add_executable(strimer_probe tools/strimer_probe.cpp src/LianLiWirelessProtocol.cpp)
target_include_directories(strimer_probe PRIVATE src "${LIBUSB_DIR}/include")
if(MSVC)
    target_link_libraries(strimer_probe PRIVATE "${LIBUSB_DIR}/VS2022/MS64/static/libusb-1.0.lib")
else()
    target_link_libraries(strimer_probe PRIVATE ${LIBUSB_LIBRARIES})
endif()
