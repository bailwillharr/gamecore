cmake_minimum_required(VERSION 3.24)

set(CMAKE_CONFIGURATION_TYPES "Debug;Release;RelWithDebInfo")

# spdlog
set(SPDLOG_BUILD_SHARED OFF)
set(SPDLOG_SYSTEM_INCLUDES ON)
set(SPDLOG_USE_STD_FORMAT ON) # uses builtin fmt and C++20
set(SPDLOG_NO_ATOMIC_LEVELS ON) # log levels aren't modified concurrently
add_subdirectory(vendor/spdlog EXCLUDE_FROM_ALL)

# SDL3
set(SDL_TESTS OFF)
set(SDL_EXAMPLES OFF)
set(SDL_SHARED ON)
set(SDL_STATIC OFF)
set(SDL_TEST_LIBRARY OFF)
set(SDL_DISABLE_INSTALL ON)
set(SDL_DISABLE_INSTALL_DOCS ON)
set(SDL_INSTALL_TESTS OFF)
set(SDL_LIBC ON)
add_subdirectory(vendor/SDL EXCLUDE_FROM_ALL)

# options:
# GC_DEV_BUILD (ON/OFF) - enable trace/debug logging, asserts, etc.
add_subdirectory(gamecore)

add_subdirectory(gamecore_template)

add_subdirectory(tools/gcpak_editor)