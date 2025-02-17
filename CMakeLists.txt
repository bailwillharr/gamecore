cmake_minimum_required(VERSION 3.25)

project(gamecore_repo)

set(CMAKE_CONFIGURATION_TYPES "Debug;Release;RelWithDebInfo")

# download CPM.cmake
file(
  DOWNLOAD
  https://github.com/cpm-cmake/CPM.cmake/releases/download/v0.40.5/CPM.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/cmake/CPM.cmake
  EXPECTED_HASH SHA256=c46b876ae3b9f994b4f05a4c15553e0485636862064f1fcc9d8b4f832086bc5d
)
include(${CMAKE_CURRENT_BINARY_DIR}/cmake/CPM.cmake)

# spdlog
CPMAddPackage(
	NAME spdlog
	VERSION 1.15.1
	URL https://github.com/gabime/spdlog/archive/refs/tags/v1.15.1.zip
	OPTIONS
		"SPDLOG_BUILD_SHARED OFF"
		"SPDLOG_SYSTEM_INCLUDES ON"
		"SPDLOG_USE_STD_FORMAT ON" # uses builtin fmt and C++20
		"SPDLOG_NO_ATOMIC_LEVELS ON" # log levels aren't modified concurrently
	EXCLUDE_FROM_ALL
	SYSTEM
)

# SDL3
CPMAddPackage(
	NAME SDL3
	VERSION 3.2.4
	URL https://github.com/libsdl-org/SDL/archive/refs/tags/release-3.2.4.zip
	EXCLUDE_FROM_ALL
	SYSTEM
)

# Volk
CPMAddPackage(
	NAME volk
	VERSION 1.4.304
	URL https://github.com/zeux/volk/archive/refs/tags/1.4.304.zip
	OPTIONS
		"VOLK_PULL_IN_VULKAN ON"
	EXCLUDE_FROM_ALL
	SYSTEM
)

# VulkanMemoryAllocator
CPMAddPackage(
	NAME VulkanMemoryAllocator
	VERSION 3.2.1
	URL https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/archive/refs/tags/v3.2.1.zip
	EXCLUDE_FROM_ALL
	SYSTEM
)

# Tracy
CPMAddPackage(
	NAME tracy
	VERSION 0.11.1
	URL https://github.com/wolfpld/tracy/archive/refs/tags/v0.11.1.zip
	OPTIONS
		"TRACY_ENABLE ON"
		"TRACY_ON_DEMAND OFF"
	EXCLUDE_FROM_ALL
	SYSTEM
)

# asio (What a mess...)
CPMAddPackage(
  NAME asio
  VERSION 1.16.1
  URL https://github.com/chriskohlhoff/asio/archive/asio-1-32-0.zip
)
add_library(asio INTERFACE)
add_library(asio::asio ALIAS asio)
target_include_directories(asio INTERFACE ${asio_SOURCE_DIR}/asio/include)
target_compile_definitions(asio INTERFACE ASIO_STANDALONE ASIO_NO_DEPRECATED)
if(WIN32)
    # macro see @ https://stackoverflow.com/a/40217291/1746503
    macro(get_win32_winnt version)
    if (CMAKE_SYSTEM_VERSION)
        set(ver ${CMAKE_SYSTEM_VERSION})
        string(REGEX MATCH "^([0-9]+).([0-9])" ver ${ver})
        string(REGEX MATCH "^([0-9]+)" verMajor ${ver})
        if ("${verMajor}" MATCHES "10")
            set(verMajor "A")
            string(REGEX REPLACE "^([0-9]+)" ${verMajor} ver ${ver})
        endif ("${verMajor}" MATCHES "10")
        string(REPLACE "." "" ver ${ver})
        string(REGEX REPLACE "([0-9A-Z])" "0\\1" ver ${ver})
        set(${version} "0x${ver}")
    endif()
    endmacro()
    if(NOT DEFINED _WIN32_WINNT)
        get_win32_winnt(ver)
        set(_WIN32_WINNT ${ver})
    endif()
    message(STATUS "Set _WIN32_WINNET=${_WIN32_WINNT}")
    target_compile_definitions(asio INTERFACE _WIN32_WINNT=${_WIN32_WINNT} WIN32_LEAN_AND_MEAN)
endif()

# shaderc_combined (from Vulkan SDK)
find_package(Vulkan REQUIRED COMPONENTS shaderc_combined)

add_subdirectory(gamecore)

add_subdirectory(gamecore_template)

add_subdirectory(dedicated_server)

add_subdirectory(tools/gcpak_editor)
