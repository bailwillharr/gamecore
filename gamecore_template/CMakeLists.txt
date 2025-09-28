cmake_minimum_required(VERSION 3.25)

project(gamecore_template LANGUAGES CXX VERSION 0.1.0)

set(SRC_FILES
  "src/main.cpp"
  "src/game.h"
  "src/game.cpp"
  "src/spin.h"
  "src/spin.cpp"
  "src/mouse_move.h"
  "src/mouse_move.cpp"
  "src/gen_mesh.h"
  "src/gen_mesh.cpp"
)

if(WIN32)
    option(GAMECORE_TEMPLATE_WIN32_BUILD "Build as a standalone win32 app (no console window)" OFF)
endif()

if(WIN32 AND GAMECORE_TEMPLATE_WIN32_BUILD)
    add_executable(${PROJECT_NAME} WIN32
        ${SRC_FILES}
        "resource/gamecore_template.rc"
    )
else()
    add_executable(${PROJECT_NAME} ${SRC_FILES})
endif()

configure_file(gamecore_template.h.in gamecore_template.h @ONLY)

# compiling options:

target_include_directories(${PROJECT_NAME} PRIVATE ${PROJECT_BINARY_DIR})

if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4 /WX)
endif()

if(WIN32)
    # stop windows.h conflicting with 'std::max'
    target_compile_definitions(${PROJECT_NAME} PRIVATE NOMINMAX)
endif()

# This project uses C++20
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD 20)
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD_REQUIRED ON)
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_EXTENSIONS OFF)

target_link_libraries(${PROJECT_NAME} PRIVATE gamecore)

target_link_libraries(${PROJECT_NAME} PUBLIC mikktspace)
target_link_libraries(${PROJECT_NAME} PUBLIC weldmesh)

if(WIN32)
    add_custom_command(
        TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy
        $<TARGET_FILE:SDL3::SDL3-shared> $<TARGET_FILE_DIR:${PROJECT_NAME}>
        VERBATIM
    )
endif()

# link game content directory to output directory
add_custom_command(
  TARGET ${PROJECT_NAME} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E create_symlink
  ${PROJECT_SOURCE_DIR}/../content $<TARGET_FILE_DIR:${PROJECT_NAME}>/content
  VERBATIM
)
