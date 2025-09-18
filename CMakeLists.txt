cmake_minimum_required(VERSION 3.25)

project(gamecore_repo)

set(CMAKE_CONFIGURATION_TYPES "Debug;Release;RelWithDebInfo")

# Enable link-time optimisation for release builds
# Compiler options such as compiler warnings, treating warnings as errors,
# are defined per project/target
if(MSVC)
    set(CMAKE_CXX_FLAGS "/DWIN32 /D_WINDOWS /D_HAS_EXCEPTIONS=1 /EHsc")
    set(CMAKE_CXX_FLAGS_DEBUG "/Ob0 /Od /RTC1")
    set(CMAKE_CXX_FLAGS_RELEASE "/O2 /Ob2 /Oi /Ot /arch:AVX2 /DNDEBUG /GL")
    set(CMAKE_CXX_FLAGS_REL_WITH_DEB_INFO "/O2 /Ob1 /DNDEBUG")
    set(CMAKE_C_FLAGS "/DWIN32 /D_WINDOWS")
    set(CMAKE_C_FLAGS_DEBUG "/Ob0 /Od /RTC1")
    set(CMAKE_C_FLAGS_RELEASE "/O2 /Ob2 /DNDEBUG /GL")
    set(CMAKE_C_FLAGS_REL_WITH_DEB_INFO "/O2 /Ob1 /DNDEBUG")
    set(CMAKE_EXE_LINKER_FLAGS "/machine:x64")
    set(CMAKE_EXE_LINKER_FLAGS_DEBUG "/debug /INCREMENTAL")
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE "/INCREMENTAL:NO /OPT:REF /LTCG:STATUS")
    set(CMAKE_EXE_LINKER_FLAGS_REL_WITH_DEB_INFO "/debug /INCREMENTAL")
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# download CPM.cmake
file(
  DOWNLOAD
  https://github.com/cpm-cmake/CPM.cmake/releases/download/v0.40.5/CPM.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/cmake/CPM.cmake
  EXPECTED_HASH
    SHA256=c46b876ae3b9f994b4f05a4c15553e0485636862064f1fcc9d8b4f832086bc5d
)
include(${CMAKE_CURRENT_BINARY_DIR}/cmake/CPM.cmake)

# spdlog
cpmaddpackage(
  NAME spdlog
  VERSION 1.15.1
  URL https://github.com/gabime/spdlog/archive/refs/tags/v1.15.1.zip
  OPTIONS
    "SPDLOG_BUILD_SHARED OFF"
    "SPDLOG_SYSTEM_INCLUDES ON"
    "SPDLOG_USE_STD_FORMAT ON" # uses builtin fmt and C++20
    "SPDLOG_NO_ATOMIC_LEVELS ON" # log levels aren't modified concurrently
  EXCLUDE_FROM_ALL YES
  SYSTEM YES
)

# SDL3
cpmaddpackage(
  NAME SDL3
  VERSION 3.2.4
  URL https://github.com/libsdl-org/SDL/archive/refs/tags/release-3.2.4.zip
  EXCLUDE_FROM_ALL YES
  SYSTEM YES
)

# Volk
cpmaddpackage(
  NAME volk
  VERSION 1.4.304
  URL https://github.com/zeux/volk/archive/refs/tags/1.4.304.zip
  OPTIONS
    "VOLK_PULL_IN_VULKAN ON"
  EXCLUDE_FROM_ALL YES
  SYSTEM YES
)

# VulkanMemoryAllocator
cpmaddpackage(
  NAME VulkanMemoryAllocator
  VERSION 3.2.1
  URL https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/archive/refs/tags/v3.2.1.zip
  EXCLUDE_FROM_ALL YES
  SYSTEM YES
)

# Tracy
if(GC_PROFILING)
    set(GC_TRACY_ENABLE "TRACY_ENABLE ON")
else()
    set(GC_TRACY_ENABLE "TRACY_ENABLE OFF")
endif()
cpmaddpackage(
  NAME tracy
  VERSION 0.12.2
  URL https://github.com/wolfpld/tracy/archive/refs/tags/v0.12.2.zip
  OPTIONS
    "${GC_TRACY_ENABLE}"
    "TRACY_ON_DEMAND OFF"
  EXCLUDE_FROM_ALL YES
  SYSTEM YES
)

# asio (What a mess...)
cpmaddpackage(
  NAME asio
  VERSION 1.16.1
  URL https://github.com/chriskohlhoff/asio/archive/asio-1-32-0.zip
)
add_library(asio INTERFACE)
add_library(asio::asio ALIAS asio)
target_include_directories(asio SYSTEM INTERFACE
  ${asio_SOURCE_DIR}/asio/include
)
target_compile_definitions(asio INTERFACE
  ASIO_STANDALONE
  ASIO_NO_DEPRECATED
  ASIO_NO_EXCEPTIONS
  ASIO_SEPARATE_COMPILATION
)
if(WIN32)
    macro(get_win32_winnt version)
        if(CMAKE_SYSTEM_VERSION)
            set(ver ${CMAKE_SYSTEM_VERSION})
            string(REGEX MATCH "^([0-9]+).([0-9])" ver ${ver})
            string(REGEX MATCH "^([0-9]+)" verMajor ${ver})
            # Check for Windows 10, b/c we'll need to convert to hex 'A'.
            if("${verMajor}" MATCHES "10")
                set(verMajor "A")
                string(REGEX REPLACE "^([0-9]+)" ${verMajor} ver ${ver})
            endif()
            # Remove all remaining '.' characters.
            string(REPLACE "." "" ver ${ver})
            # Prepend each digit with a zero.
            string(REGEX REPLACE "([0-9A-Z])" "0\\1" ver ${ver})
            set(${version} "0x${ver}")
        endif()
    endmacro()

    get_win32_winnt(ver)
    target_compile_definitions(asio INTERFACE _WIN32_WINNT=${ver})
endif()

# json
cpmaddpackage(
  NAME json
  VERSION 3.11.3
  URL https://github.com/nlohmann/json/archive/refs/tags/v3.11.3.zip
  OPTIONS
    "JSON_SystemInclude ON"
  EXCLUDE_FROM_ALL YES
  SYSTEM YES
)

# glm
cpmaddpackage(
  NAME glm
  VERSION 1.0.1
  URL https://github.com/g-truc/glm/releases/download/1.0.1/glm-1.0.1-light.zip
  DOWNLOAD_ONLY YES # built-in CMakeLists.txt doesn't work
)
add_library(glm INTERFACE)
add_library(glm::glm ALIAS glm)
target_include_directories(glm SYSTEM INTERFACE ${glm_SOURCE_DIR})

# ImGui
cpmaddpackage(
  NAME imgui
  VERSION 1.91.9
  GITHUB_REPOSITORY ocornut/imgui
  DOWNLOAD_ONLY YES
)
file(
  DOWNLOAD
  "https://gist.githubusercontent.com/rokups/f771217b2d530d170db5cb1e08e9a8f4/raw/4c2c14374ab878ca2f45daabfed4c156468e4e27/CMakeLists.txt"
  "${imgui_SOURCE_DIR}/CMakeLists.txt"
  EXPECTED_HASH SHA256=fd62f69364ce13a4f7633a9b50ae6672c466bcc44be60c69c45c0c6e225bb086
)
add_library(imgui STATIC
  "${imgui_SOURCE_DIR}/imconfig.h"
  "${imgui_SOURCE_DIR}/imgui.cpp"
  "${imgui_SOURCE_DIR}/imgui.h"
  "${imgui_SOURCE_DIR}/imgui_demo.cpp"
  "${imgui_SOURCE_DIR}/imgui_draw.cpp"
  "${imgui_SOURCE_DIR}/imgui_internal.h"
  "${imgui_SOURCE_DIR}/imgui_tables.cpp"
  "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
  "${imgui_SOURCE_DIR}/imstb_rectpack.h"
  "${imgui_SOURCE_DIR}/imstb_textedit.h"
  "${imgui_SOURCE_DIR}/imstb_truetype.h"
  "${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp"
  "${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.h"
  "${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp"
  "${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.h"
)
target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
# use ImGui_ImplVulkan_LoadFunctions():
target_compile_definitions(imgui PUBLIC IMGUI_IMPL_VULKAN_NO_PROTOTYPES)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR})
target_link_libraries(imgui PRIVATE volk::volk)
target_link_libraries(imgui PRIVATE SDL3::SDL3)

# tinygltf
cpmaddpackage(
  NAME tinygltf
  VERSION 2.9.6
  URL https://github.com/syoyo/tinygltf/archive/refs/tags/v2.9.6.zip
  OPTIONS
    "TINYGLTF_BUILD_LOADER_EXAMPLE OFF"
    "TINYGLTF_HEADER_ONLY OFF"
    "TINYGLTF_INSTALL OFF"
    "TINYGLTF_INSTALL_VENDOR OFF"
  EXCLUDE_FROM_ALL YES
  SYSTEM YES
)
# tinygltf has its own json.hpp and stb_image_ libraries
target_compile_definitions(tinygltf PUBLIC TINYGLTF_NO_EXTERNAL_IMAGE)

# mio
cpmaddpackage(
    NAME mio
    URL https://github.com/vimpunk/mio/archive/8b6b7d878c89e81614d05edca7936de41ccdd2da.zip
    EXCLUDE_FROM_ALL YES
    SYSTEM YES
)

# shaderc_combined (from Vulkan SDK)
find_package(Vulkan REQUIRED COMPONENTS shaderc_combined REQUIRED)

# library for manipulating game asset files
add_subdirectory(gcpak)

add_subdirectory(gamecore)

add_subdirectory(gamecore_template)

#add_subdirectory(dedicated_server)

#add_subdirectory(tools/gcpak_editor) # broken
add_subdirectory(tools/compile_shaders)
