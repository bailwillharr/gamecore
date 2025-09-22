cmake_minimum_required(VERSION 3.25)

project(compile_shaders LANGUAGES CXX)

set(SRC_FILES
  "src/main.cpp"
)

add_executable(${PROJECT_NAME}
  ${SRC_FILES}
)

configure_file(compile_shaders.h.in compile_shaders.h @ONLY)

# compiling options:

target_include_directories(${PROJECT_NAME} PRIVATE ${PROJECT_BINARY_DIR})

if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4 /WX)
endif()

if(WIN32)
    # stop windows.h conflicting with 'std::max'
    target_compile_definitions(${PROJECT_NAME} PRIVATE NOMINMAX)
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE gcpak)

if(LINUX)
    target_link_libraries(${PROJECT_NAME} PRIVATE shaderc_shared)
else()
    target_link_libraries(${PROJECT_NAME} PRIVATE Vulkan::shaderc_combined)
endif()

# This project uses C++20
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD 20)
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD_REQUIRED ON)
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_EXTENSIONS OFF)
