cmake_minimum_required(VERSION 3.25)

project(ember_court LANGUAGES CXX VERSION 0.1.0)

# The game itself is a library, so that the map generator can use its components
set(GAME_SRC_FILES
  "src/game.h"
  "src/game.cpp"
  "src/components.h"
  "src/character.h"
  "src/character.cpp"
  "src/match.h"
  "src/match.cpp"
  "src/player_control.h"
  "src/player_control.cpp"
  "src/bot.h"
  "src/bot.cpp"
  "src/visuals.h"
  "src/visuals.cpp"
  "src/hud.h"
  "src/hud.cpp"
)

add_library(${PROJECT_NAME}_game STATIC ${GAME_SRC_FILES})
target_include_directories(${PROJECT_NAME}_game PUBLIC src)
target_link_libraries(${PROJECT_NAME}_game PUBLIC gamecore)

if(WIN32)
    add_executable(${PROJECT_NAME} WIN32 "src/main.cpp")
else()
    add_executable(${PROJECT_NAME} "src/main.cpp")
endif()
target_link_libraries(${PROJECT_NAME} PRIVATE ${PROJECT_NAME}_game)

# Makes the map. Always a console application so that what it did is printed
add_executable(${PROJECT_NAME}_mapgen "mapgen/main.cpp")
target_link_libraries(${PROJECT_NAME}_mapgen PRIVATE ${PROJECT_NAME}_game)

# compiling options:

foreach(target ${PROJECT_NAME}_game ${PROJECT_NAME} ${PROJECT_NAME}_mapgen)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX)
    endif()

    if(WIN32)
        # stop windows.h conflicting with 'std::max'
        target_compile_definitions(${target} PRIVATE NOMINMAX)
    endif()

    # This project uses C++20
    target_compile_features(${target} PUBLIC cxx_std_20)
    set_target_properties(${target} PROPERTIES
      CXX_STANDARD 20
      CXX_STANDARD_REQUIRED YES
      CXX_EXTENSIONS NO
    )
endforeach()

foreach(target ${PROJECT_NAME} ${PROJECT_NAME}_mapgen)
    if(WIN32)
        add_custom_command(
            TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy
            $<TARGET_FILE:SDL3::SDL3-shared> $<TARGET_FILE_DIR:${target}>
            VERBATIM
        )
    endif()
endforeach()

# link game content directory to output directory
add_custom_command(
  TARGET ${PROJECT_NAME} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E create_symlink
  ${PROJECT_SOURCE_DIR}/../content $<TARGET_FILE_DIR:${PROJECT_NAME}>/content
  VERBATIM
)

# The map: content/ember_court.gcpak. The map generator makes the prefab and its meshes, materials and textures, then gcpak_editor
# bakes the prefab's shadow map into the same file. Made again whenever the map generator or gcpak_editor changes.
set(EMBER_COURT_MAP "${PROJECT_SOURCE_DIR}/../content/ember_court.gcpak")
add_custom_command(
  OUTPUT ${EMBER_COURT_MAP}
  COMMAND ${PROJECT_NAME}_mapgen ${EMBER_COURT_MAP}
  COMMAND gcpak_editor --bake-shadows ember_court.map ${EMBER_COURT_MAP} --no-cast sky --no-cast beacon
  DEPENDS ${PROJECT_NAME}_mapgen gcpak_editor
  COMMENT "Generating the Ember Court map and baking its shadows"
  VERBATIM
)
add_custom_target(${PROJECT_NAME}_map ALL DEPENDS ${EMBER_COURT_MAP})
add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}_map)
