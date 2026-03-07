cmake_minimum_required(VERSION 3.25)

project(package_textures LANGUAGES CXX)

set(SRC_FILES
  "src/main.cpp"
)

add_executable(${PROJECT_NAME}
  ${SRC_FILES}
)

configure_file(package_textures.h.in package_textures.h @ONLY)

# compiling options:

target_include_directories(${PROJECT_NAME} PRIVATE ${PROJECT_BINARY_DIR})

if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4)
endif()

if(WIN32)
    # stop windows.h conflicting with 'std::max'
    target_compile_definitions(${PROJECT_NAME} PRIVATE NOMINMAX)
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE gcpak)

target_link_libraries(${PROJECT_NAME} PRIVATE stb)

# This project uses C++20
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_20)