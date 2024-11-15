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
  "src/gc_logger_null.cpp"
  "src/gc_logger_spdlog.cpp"
  "src/gc_jobs.cpp"
)

# Public API includes
# Care must be taken to ensure these headers do not reference private headers from src/
set(INCLUDE_FILES
	"include/gamecore/gc_app.h"
  "include/gamecore/gc_assert.h"
  "include/gamecore/gc_logger.h"
  "include/gamecore/gc_logger_null.h"
  "include/gamecore/gc_logger_spdlog.h"
  "include/gamecore/gc_jobs.h"
  "include/gamecore/gc_ring_buffer.h"
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

# spdlog
set(SPDLOG_BUILD_SHARED OFF)
set(SPDLOG_SYSTEM_INCLUDES ON)
set(SPDLOG_USE_STD_FORMAT ON) # uses builtin fmt and C++20
add_subdirectory(vendor/spdlog)
target_link_libraries(${PROJECT_NAME} PUBLIC spdlog::spdlog)
