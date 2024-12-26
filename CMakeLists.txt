cmake_minimum_required(VERSION 3.24)

set(CMAKE_CONFIGURATION_TYPES "Debug;Release;RelWithDebInfo")

# spdlog
set(SPDLOG_BUILD_SHARED OFF)
set(SPDLOG_SYSTEM_INCLUDES ON)
set(SPDLOG_USE_STD_FORMAT ON) # uses builtin fmt and C++20
add_subdirectory(vendor/spdlog)

# options:
# GC_DEV_BUILD (ON/OFF) - enable trace/debug logging, asserts, etc.
add_subdirectory(gamecore)

add_subdirectory(gamecore_test)

add_subdirectory(tools/gcpak_editor)