cmake_minimum_required(VERSION 3.25)

project(gamecore_repo)

set(CMAKE_CONFIGURATION_TYPES "Debug;Release;RelWithDebInfo")

include(FetchContent)

# spdlog
set(SPDLOG_BUILD_SHARED OFF CACHE INTERNAL "" FORCE)
set(SPDLOG_SYSTEM_INCLUDES ON CACHE INTERNAL "" FORCE)
set(SPDLOG_USE_STD_FORMAT ON CACHE INTERNAL "" FORCE) # uses builtin fmt and C++20
set(SPDLOG_NO_ATOMIC_LEVELS ON CACHE INTERNAL "" FORCE) # log levels aren't modified concurrently
add_subdirectory(vendor/spdlog EXCLUDE_FROM_ALL)

# SDL3

FetchContent_Declare(
	SDL3
	GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
	GIT_TAG release-3.2.4
	FIND_PACKAGE_ARGS
)

FetchContent_MakeAvailable(SDL3)

# Volk
FetchContent_Declare(
	volk
	GIT_REPOSITORY https://github.com/zeux/volk.git
	GIT_TAG vulkan-sdk-1.4.304.1
	FIND_PACKAGE_ARGS
)
set(VOLK_PULL_IN_VULKAN ON CACHE INTERNAL "" FORCE) # Get Vulkan header files from SDK
if (WIN32)
	set(VOLK_STATIC_DEFINES "VK_USE_PLATFORM_WIN32_KHR" CACHE INTERNAL "" FORCE)
endif()
FetchContent_MakeAvailable(volk)

# VulkanMemoryAllocator
FetchContent_Declare(
	VulkanMemoryAllocator
	GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
	GIT_TAG v3.2.1
)
FetchContent_MakeAvailable(VulkanMemoryAllocator)

# Tracy
FetchContent_Declare(
	tracy
	GIT_REPOSITORY https://github.com/wolfpld/tracy.git
	GIT_TAG v0.11.1
)
if (GC_PROFILING)
set(TRACY_ENABLE ON CACHE INTERNAL "" FORCE)
else()
set(TRACY_ENABLE OFF CACHE INTERNAL "" FORCE)
endif()
set(TRACY_ON_DEMAND OFF CACHE INTERNAL "" FORCE)
FetchContent_MakeAvailable(tracy)

# shaderc_combined (from Vulkan SDK)
find_package(Vulkan COMPONENTS shaderc_combined)

add_subdirectory(gamecore)

add_subdirectory(gamecore_template)

add_subdirectory(dedicated_server)

add_subdirectory(tools/gcpak_editor)
