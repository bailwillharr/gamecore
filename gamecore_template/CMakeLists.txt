cmake_minimum_required(VERSION 3.25)

project(gamecore_template LANGUAGES CXX VERSION 0.1.0)

# The game itself is a library so that the dedicated server can run it too
set(GAME_SRC_FILES
  "src/game.h"
  "src/game.cpp"
  "src/arena.h"
  "src/arena.cpp"
  "src/bot.h"
  "src/bot.cpp"
  "src/hud.h"
  "src/hud.cpp"
  "src/spin.h"
  "src/spin.cpp"
  "src/mouse_move.h"
  "src/mouse_move.cpp"
)

add_library(${PROJECT_NAME}_game STATIC ${GAME_SRC_FILES})
target_include_directories(${PROJECT_NAME}_game PUBLIC src)
target_link_libraries(${PROJECT_NAME}_game PUBLIC gamecore)
target_compile_features(${PROJECT_NAME}_game PUBLIC cxx_std_20)
set_target_properties(${PROJECT_NAME}_game PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED YES
  CXX_EXTENSIONS NO
)
if(MSVC)
    target_compile_options(${PROJECT_NAME}_game PRIVATE /W4 /WX)
endif()
if(WIN32)
    target_compile_definitions(${PROJECT_NAME}_game PRIVATE NOMINMAX)
endif()

set(SRC_FILES
  "src/main.cpp"
)

if(WIN32)
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
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_20)
set_target_properties(${PROJECT_NAME} PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED YES
  CXX_EXTENSIONS NO
)

target_link_libraries(${PROJECT_NAME} PRIVATE ${PROJECT_NAME}_game)

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
