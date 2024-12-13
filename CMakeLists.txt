cmake_minimum_required(VERSION 3.24)

# options
option(GC_DEV_BUILD "Log trace and debug messages, run asserts" ON)

set(CMAKE_CONFIGURATION_TYPES "Debug;Release;RelWithDebInfo")

project(gamecore LANGUAGES CXX
	VERSION "0.1.0"
)

# from command: find . -regex "^\.\/.*" | sort
# Header files can also be here if they're not part of the public API
set(SRC_FILES
	"src/gc_app.cpp"
	"src/gc_assert.cpp"
	"src/gc_logger.cpp"
	"src/gc_logger_spdlog.cpp"
	"src/gc_jobs.cpp"
	"src/gc_content.cpp"
	"src/gc_abort.cpp"
	"src/gc_disk_io.cpp"
)

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
	"include/gamecore/gc_disk_io.h"
	"include/gamecore/gc_crc_table.h"
	"include/gamecore/gc_asset_id.h"
	"include/gamecore/gc_gcpak.h"
	"include/gamecore/gc_stopwatch.h"
)

# gamecore is a static library
add_library(${PROJECT_NAME} STATIC
	${SRC_FILES}
	${INCLUDE_FILES}
)

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" PREFIX "Source" FILES ${SRC_FILES})
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/include" PREFIX "Include" FILES ${INCLUDE_FILES})

# compiling options:

if (WIN32)
	target_compile_definitions(${PROJECT_NAME} PRIVATE NOMINMAX) # stop windows.h conflicting with 'std::max'
endif()

if (GC_DEV_BUILD)
  target_compile_definitions(${PROJECT_NAME} PRIVATE GC_DO_ASSERTS) # gc_assert.h
  target_compile_definitions(${PROJECT_NAME} PRIVATE GC_LOG_TRACE_DEBUG) # gc_logger.cpp
endif()
target_compile_definitions(${PROJECT_NAME} PRIVATE GC_LOGGER=GC_LOGGER_SPDLOG) # GC_LOGGER_SPDLOG or GC_LOGGER_NULL

# This project uses C++20
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD 20)
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD_REQUIRED ON)
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_EXTENSIONS OFF)

# compiler warnings
if (MSVC)
	target_compile_options(${PROJECT_NAME} PRIVATE /W3)
	target_compile_options(${PROJECT_NAME} PRIVATE /MP)
	target_compile_definitions(${PROJECT_NAME} PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
	target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -pedantic)
endif()

target_include_directories(${PROJECT_NAME} PRIVATE include)
target_include_directories(${PROJECT_NAME} PRIVATE src)

# add gcpak editor
add_subdirectory(tools/gcpak_editor)

# test executable for debugging
add_executable(${PROJECT_NAME}_test
  "test/main.cpp"
)
target_link_libraries(${PROJECT_NAME}_test PRIVATE ${PROJECT_NAME})
target_include_directories(${PROJECT_NAME}_test PRIVATE include)
set_property(TARGET ${PROJECT_NAME}_test PROPERTY CXX_STANDARD 20)
set_property(TARGET ${PROJECT_NAME}_test PROPERTY CXX_STANDARD_REQUIRED ON)
set_property(TARGET ${PROJECT_NAME}_test PROPERTY CXX_EXTENSIONS OFF)
if (MSVC)
	target_compile_options(${PROJECT_NAME}_test PRIVATE /W3)
	target_compile_options(${PROJECT_NAME}_test PRIVATE /MP)
	target_compile_definitions(${PROJECT_NAME}_test PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
	target_compile_options(${PROJECT_NAME}_test PRIVATE -Wall -Wextra -pedantic)
endif()
if (WIN32)
	target_compile_definitions(${PROJECT_NAME}_test PRIVATE NOMINMAX) # stop windows.h conflicting with 'std::max'
endif()
if (GC_DEV_BUILD)
  target_compile_definitions(${PROJECT_NAME}_test PRIVATE GC_DO_ASSERTS) # gc_assert.h
  target_compile_definitions(${PROJECT_NAME}_test PRIVATE GC_LOG_TRACE_DEBUG) # gc_logger.cpp
endif()
target_compile_definitions(${PROJECT_NAME}_test PRIVATE GC_LOGGER=GC_LOGGER_SPDLOG) # GC_LOGGER_SPDLOG or GC_LOGGER_NULL
# Copy test resources to the output directory
add_custom_command(
    TARGET ${PROJECT_NAME}_test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/test/content"
            "${CMAKE_BINARY_DIR}/content"
    COMMENT "Copying content directory to the output binary directory"
)

# spdlog
set(SPDLOG_BUILD_SHARED OFF)
set(SPDLOG_SYSTEM_INCLUDES ON)
set(SPDLOG_USE_STD_FORMAT ON) # uses builtin fmt and C++20
add_subdirectory(vendor/spdlog)
target_link_libraries(${PROJECT_NAME} PUBLIC spdlog::spdlog)
