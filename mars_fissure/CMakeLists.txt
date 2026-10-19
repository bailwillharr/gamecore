cmake_minimum_required(VERSION 3.25)

project(mars_fissure LANGUAGES CXX VERSION 0.1.0)

set(SRC_FILES
  "src/main.cpp"
  "src/game.h"
  "src/game.cpp"
  "src/components.h"
  "src/content_names.h"
  "src/explorer.h"
  "src/explorer.cpp"
  "src/beacons.h"
  "src/beacons.cpp"
  "src/flythrough.h"
  "src/flythrough.cpp"
  "src/hud.h"
  "src/hud.cpp"
)

if(WIN32)
    add_executable(${PROJECT_NAME} WIN32 ${SRC_FILES})
else()
    add_executable(${PROJECT_NAME} ${SRC_FILES})
endif()
target_link_libraries(${PROJECT_NAME} PRIVATE gamecore)

# Makes the ground's material and the sky's panorama. Always a console application so that what it did is printed
add_executable(${PROJECT_NAME}_contentgen "contentgen/main.cpp")
target_include_directories(${PROJECT_NAME}_contentgen PRIVATE src)
target_link_libraries(${PROJECT_NAME}_contentgen PRIVATE gamecore)

# compiling options:

foreach(target ${PROJECT_NAME} ${PROJECT_NAME}_contentgen)
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

# content/mars.gcpak: the ground's material and textures, and the sky. Made again whenever the content generator or gcpak_editor
# changes (the sky's cube maps are baked by gcpak_editor, from the panorama that the generator makes).
set(MARS_CONTENT "${PROJECT_SOURCE_DIR}/../content/mars.gcpak")
set(MARS_SKY "${CMAKE_CURRENT_BINARY_DIR}/mars_sky.hdr")
add_custom_command(
  OUTPUT ${MARS_CONTENT}
  COMMAND ${PROJECT_NAME}_contentgen ${MARS_CONTENT} ${MARS_SKY}
  COMMAND gcpak_editor --pack ${MARS_SKY} --out ${MARS_CONTENT} --append
  DEPENDS ${PROJECT_NAME}_contentgen gcpak_editor
  COMMENT "Making Mars Fissure's sky and ground"
  VERBATIM
)
add_custom_target(${PROJECT_NAME}_content ALL DEPENDS ${MARS_CONTENT})
add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}_content)

# content/mars_terrain.gcpak: the terrain, baked from a HiRISE digital terrain model, which isn't in the repository (it is 300 MB).
# Download DTEEC_046252_1975_046107_1975_A01.IMG from https://www.uahirise.org/dtm/ (or the PDS) and set MARS_DTM to where it is.
# It is baked again only when the DTM or the terrain baker's code changes (it takes a few seconds in a release build).
set(MARS_DTM "C:/blender/enginedev/mars/fissure/DTEEC_046252_1975_046107_1975_A01.IMG" CACHE FILEPATH
    "The HiRISE DTM that Mars Fissure's terrain is baked from")
set(MARS_TERRAIN "${PROJECT_SOURCE_DIR}/../content/mars_terrain.gcpak")
if(EXISTS "${MARS_DTM}")
    add_custom_command(
      OUTPUT ${MARS_TERRAIN}
      COMMAND gcpak_editor --bake-terrain ${MARS_DTM} --out ${MARS_TERRAIN} --name mars_fissure.terrain
      DEPENDS
        ${MARS_DTM}
        "${PROJECT_SOURCE_DIR}/../tools/gcpak_editor/src/terrain_compiler.cpp"
        "${PROJECT_SOURCE_DIR}/../gamecore/src/gc_terrain_builder.cpp"
        "${PROJECT_SOURCE_DIR}/../gamecore/include/gamecore/gc_terrain.h"
      COMMENT "Baking Mars Fissure's terrain from ${MARS_DTM}"
      VERBATIM
    )
    add_custom_target(${PROJECT_NAME}_terrain ALL DEPENDS ${MARS_TERRAIN})
    add_dependencies(${PROJECT_NAME}_terrain gcpak_editor)
    add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}_terrain)
else()
    message(STATUS "mars_fissure: the DTM isn't at ${MARS_DTM}, so the terrain won't be baked. Set MARS_DTM (see mars_fissure/README)")
endif()
