cmake_minimum_required(VERSION 3.25)

option(GC_DEV_BUILD "run asserts, check for hash collisions" ON)

project(gamecore LANGUAGES CXX VERSION "0.1.0")

# from command: find . -regex "^\.\/.*" | sort
# Header files can also be here if they're not part of the public API
set(SRC_FILES
  "src/gc_app.cpp"
  "src/gc_assert.cpp"
  "src/gc_jobs.cpp"
  "src/gc_content.cpp"
  "src/gc_units.cpp"
  "src/gc_window.cpp"
  "src/gc_vulkan_device.cpp"
  "src/gc_vulkan_allocator.cpp"
  "src/gc_vk_mem_alloc_impl.cpp"
  "src/gc_render_backend.cpp"
  "src/gc_render_mesh.cpp"
  "src/gc_render_world.cpp"
  "src/gc_vulkan_swapchain.cpp"
  "src/gc_configuration.cpp"
  "src/gc_debug_ui.cpp"
  "src/gc_world.cpp"
  "src/gc_importer_gltf.cpp"
  "src/gc_ecs.cpp"
  "src/gc_transform_system.cpp"
  "src/gc_render_system.cpp"
  "src/gc_threading.cpp"
  "src/gc_vulkan_utils.cpp"
  "src/gc_gen_tangents.cpp"
  "src/gc_resource_manager.cpp"
  "src/gc_light_system.cpp"
  "src/gc_gen_mesh.cpp"
  "src/gc_render_buffer.cpp"
  "src/gc_prefab.cpp"
  "src/gc_net.cpp"
  "src/gc_net_server.cpp"
  "src/gc_net_client.cpp"
  "src/gc_net_common.cpp"
  "src/gc_net_ui.cpp"
  "src/gc_byte_reader.cpp"
  "src/gc_byte_writer.cpp"
)

# Public API includes
# Care must be taken to ensure these headers do not reference private headers
set(INCLUDE_FILES
  "include/gamecore/gc_app.h"
  "include/gamecore/gc_assert.h"
  "include/gamecore/gc_jobs.h"
  "include/gamecore/gc_ring_buffer.h"
  "include/gamecore/gc_content.h"
  "include/gamecore/gc_abort.h"
  "include/gamecore/gc_crc_table.h"
  "include/gamecore/gc_name.h"
  "include/gamecore/gc_stopwatch.h"
  "include/gamecore/gc_units.h"
  "include/gamecore/gc_window.h"
  "include/gamecore/gc_vulkan_common.h"
  "include/gamecore/gc_vulkan_device.h"
  "include/gamecore/gc_vulkan_allocator.h"
  "include/gamecore/gc_render_backend.h"
  "include/gamecore/gc_render_texture.h"
  "include/gamecore/gc_render_material.h"
  "include/gamecore/gc_render_mesh.h"
  "include/gamecore/gc_render_world.h"
  "include/gamecore/gc_vulkan_swapchain.h"
  "include/gamecore/gc_configuration.h"
  "include/gamecore/gc_debug_ui.h"
  "include/gamecore/gc_world.h"
  "include/gamecore/gc_importer_gltf.h"
  "include/gamecore/gc_ecs.h"
  "include/gamecore/gc_transform_component.h"
  "include/gamecore/gc_transform_system.h"
  "include/gamecore/gc_world_draw_data.h"
  "include/gamecore/gc_gpu_resources.h"
  "include/gamecore/gc_renderable_component.h"
  "include/gamecore/gc_render_system.h"
  "include/gamecore/gc_camera_component.h"
  "include/gamecore/gc_camera_system.h"
  "include/gamecore/gc_frame_state.h"
  "include/gamecore/gc_format_specialisations.h"
  "include/gamecore/gc_threading.h"
  "include/gamecore/gc_vulkan_utils.h"
  "include/gamecore/gc_gen_tangents.h"
  "include/gamecore/gc_resource_manager.h"
  "include/gamecore/gc_render_texture_manager.h"
  "include/gamecore/gc_render_object_manager.h"
  "include/gamecore/gc_resources.h"
  "include/gamecore/gc_light_component.h"
  "include/gamecore/gc_light_system.h"
  "include/gamecore/gc_gen_mesh.h"
  "include/gamecore/gc_render_buffer.h"
  "include/gamecore/gc_prefab.h"
  "include/gamecore/gc_net.h"
  "include/gamecore/gc_net_server.h"
  "include/gamecore/gc_net_client.h"
  "include/gamecore/gc_net_common.h"
  "include/gamecore/gc_net_ui.h"
  "include/gamecore/gc_net_rto.h"
  "include/gamecore/gc_byte_reader.h"
  "include/gamecore/gc_byte_writer.h"
)

# gamecore is a static library
add_library(${PROJECT_NAME} STATIC
  ${SRC_FILES}
  ${INCLUDE_FILES}
)

# This project uses C++20
target_compile_features(${PROJECT_NAME} PUBLIC cxx_std_20)
set_target_properties(${PROJECT_NAME} PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED YES
  CXX_EXTENSIONS NO
)

if(MSVC)
    # Lots of warnings, treat warnings as errors
    target_compile_options(${PROJECT_NAME} PRIVATE /W4 /WX)
endif()

if(WIN32)
    # stop windows.h conflicting with 'std::max'
    target_compile_definitions(${PROJECT_NAME} PRIVATE NOMINMAX)
endif()

if(GC_DEV_BUILD)
    target_compile_definitions(${PROJECT_NAME} PUBLIC GC_DO_ASSERTS)
    target_compile_definitions(${PROJECT_NAME} PUBLIC GC_LOOKUP_ASSET_IDS)
    target_compile_definitions(${PROJECT_NAME} PUBLIC GC_CHECK_COLLISIONS)
    #target_compile_definitions(${PROJECT_NAME} PUBLIC GC_VULKAN_VALIDATION)
    target_compile_definitions(${PROJECT_NAME} PUBLIC GC_LOG_FILE_CWD)
endif(GC_DEV_BUILD)

if (UNIX)
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wpedantic)
endif()

target_include_directories(${PROJECT_NAME} PUBLIC include)
target_include_directories(${PROJECT_NAME} PRIVATE src)

target_link_libraries(${PROJECT_NAME} PUBLIC gclog)
target_link_libraries(${PROJECT_NAME} PUBLIC gcpak)
target_link_libraries(${PROJECT_NAME} PUBLIC gctemplates)
target_link_libraries(${PROJECT_NAME} PUBLIC SDL3::SDL3)
target_link_libraries(${PROJECT_NAME} PUBLIC volk::volk) # pulls in Vulkan
target_link_libraries(${PROJECT_NAME} PUBLIC GPUOpen::VulkanMemoryAllocator)
target_link_libraries(${PROJECT_NAME} PUBLIC Tracy::TracyClient)
target_link_libraries(${PROJECT_NAME} PUBLIC asio::asio)
target_link_libraries(${PROJECT_NAME} PUBLIC nlohmann_json::nlohmann_json)
target_link_libraries(${PROJECT_NAME} PUBLIC glm::glm)
target_link_libraries(${PROJECT_NAME} PUBLIC imgui)
target_link_libraries(${PROJECT_NAME} PUBLIC tinygltf)
target_link_libraries(${PROJECT_NAME} PUBLIC mio::mio)
target_link_libraries(${PROJECT_NAME} PUBLIC stb)
target_link_libraries(${PROJECT_NAME} PUBLIC mikktspace)
target_link_libraries(${PROJECT_NAME} PUBLIC weldmesh)
