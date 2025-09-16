cmake_minimum_required(VERSION 3.25)

if (NOT CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    message(FATAL_ERROR "This project is not intended to be used as a subdirectory.")
endif()

option(GC_DEV_BUILD "Log trace and debug messages, run asserts" ON)

project(gamecore LANGUAGES CXX VERSION "0.1.0")

# from command: find . -regex "^\.\/.*" | sort
# Header files can also be here if they're not part of the public API
set(SRC_FILES
	"src/gc_app.cpp"
	"src/gc_assert.cpp"
	"src/gc_logger.cpp"
	"src/gc_logger_spdlog.cpp"
	"src/gc_jobs.cpp"
	"src/gc_content.cpp"
	
	"src/gc_name.cpp"
	"src/gc_units.cpp"
	"src/gc_window.cpp"
	"src/gc_vulkan_device.cpp"
	"src/gc_vulkan_allocator.cpp"
	"src/gc_vk_mem_alloc_impl.cpp"
	"src/gc_asio_impl.cpp"
	"src/gc_render_backend.cpp"
	"src/gc_vulkan_swapchain.cpp"
	"src/gc_compile_shader.cpp"
		
	"src/gc_configuration.cpp"
	"src/gc_debug_ui.cpp"
	"src/gc_world.cpp"
	"src/gc_importer_gltf.cpp"
	"src/gc_ecs.cpp"
	"src/gc_transform_system.cpp"
 "src/gc_cube_system.cpp")

# Public API includes
# Care must be taken to ensure these headers do not reference private headers from src/
set(INCLUDE_FILES
	"include/gamecore/gc_app.h"
	"include/gamecore/gc_assert.h"
	"include/gamecore/gc_logger.h"
	"include/gamecore/gc_logger_spdlog.h"
	"include/gamecore/gc_jobs.h"
	"include/gamecore/gc_ring_buffer.h"
	"include/gamecore/gc_defines.h"
	"include/gamecore/gc_content.h"
	"include/gamecore/gc_abort.h"
	"include/gamecore/gc_crc_table.h"
	"include/gamecore/gc_name.h"
	"include/gamecore/gc_gcpak.h"
	"include/gamecore/gc_stopwatch.h"
	"include/gamecore/gc_units.h"
	"include/gamecore/gc_window.h"
	"include/gamecore/gc_vulkan_common.h"
	"include/gamecore/gc_vulkan_device.h"
	"include/gamecore/gc_vulkan_allocator.h"
	"include/gamecore/gc_render_backend.h"
	"include/gamecore/gc_vulkan_swapchain.h"
	"include/gamecore/gc_compile_shader.h"
	"include/gamecore/gc_configuration.h"
	"include/gamecore/gc_debug_ui.h"
	"include/gamecore/gc_world.h"
	"include/gamecore/gc_importer_gltf.h"
	"include/gamecore/gc_ecs.h"
	"include/gamecore/gc_transform_component.h"
	"include/gamecore/gc_transform_system.h"
	"include/gamecore/gc_world_draw_data.h"
	"include/gamecore/gc_gpu_resources.h"
	"include/gamecore/gc_vulkan_pipeline_layout_manager.h"
	"include/gamecore/gc_cube_component.h"
	"include/gamecore/gc_cube_system.h"
	"include/gamecore/gc_frame_state.h"
	"include/gamecore/gc_format_specialisations.h"
)

# gamecore is a static library
add_library(${PROJECT_NAME} STATIC
	${SRC_FILES}
	${INCLUDE_FILES}
)

# This project uses C++20
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD 20)
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD_REQUIRED ON)
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_EXTENSIONS ON)

if (MSVC)
	target_compile_options(${PROJECT_NAME} PRIVATE /W4 /WX) # warning level 4 and treat warnings as errors
endif()

if (WIN32)
	target_compile_definitions(${PROJECT_NAME} PRIVATE NOMINMAX) # stop windows.h conflicting with 'std::max'
endif()

# These are public as linking executable must also have these defined for some include files
if (GC_DEV_BUILD)
	target_compile_definitions(${PROJECT_NAME} PUBLIC GC_DO_ASSERTS)
	target_compile_definitions(${PROJECT_NAME} PUBLIC GC_LOG_TRACE_DEBUG)
	target_compile_definitions(${PROJECT_NAME} PUBLIC GC_LOOKUP_ASSET_IDS)
	#target_compile_definitions(${PROJECT_NAME} PUBLIC GC_VULKAN_VALIDATION) # Vulkan Configurator can do this
endif()
target_compile_definitions(${PROJECT_NAME} PUBLIC GC_LOGGER=GC_LOGGER_SPDLOG) # GC_LOGGER_SPDLOG or GC_LOGGER_NULL

target_include_directories(${PROJECT_NAME} PUBLIC include)
target_include_directories(${PROJECT_NAME} PRIVATE src)

# spdlog
target_link_libraries(${PROJECT_NAME} PRIVATE spdlog::spdlog)

# SDL3
target_link_libraries(${PROJECT_NAME} PUBLIC SDL3::SDL3)

# Vulkan
# volk::volk pulls in Vulkan headers
target_link_libraries(${PROJECT_NAME} PUBLIC volk::volk)

# VulkanMemoryAllocator
target_link_libraries(${PROJECT_NAME} PUBLIC GPUOpen::VulkanMemoryAllocator)

# Tracy
target_link_libraries(${PROJECT_NAME} PUBLIC Tracy::TracyClient)

# asio
target_link_libraries(${PROJECT_NAME} PUBLIC asio::asio)

# json
target_link_libraries(${PROJECT_NAME} PUBLIC nlohmann_json::nlohmann_json)

# glm
target_link_libraries(${PROJECT_NAME} PUBLIC glm::glm)

# imgui
target_link_libraries(${PROJECT_NAME} PUBLIC imgui)

# tinygltf
target_link_libraries(${PROJECT_NAME} PUBLIC tinygltf)

# mio
target_link_libraries(${PROJECT_NAME} PUBLIC mio::mio)

# For some reason Vulkan::shaderc_combined doesn't link on linux even though FindVulkan.cmake finds the component??
if (WIN32)
	target_link_libraries(${PROJECT_NAME} PUBLIC Vulkan::shaderc_combined)
else()
	target_link_libraries(${PROJECT_NAME} PUBLIC shaderc_shared)
endif()
