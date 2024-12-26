cmake_minimum_required(VERSION 3.24)

option(GC_DEV_BUILD "Log trace and debug messages, run asserts" ON)

project(gamecore LANGUAGES CXX VERSION "0.1.0")

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
	"src/gc_asset_id.cpp"
	"src/gc_units.cpp"
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
	"include/gamecore/gc_units.h"
)

# gamecore is a static library
add_library(${PROJECT_NAME} STATIC
	${SRC_FILES}
	${INCLUDE_FILES}
)

# This project uses C++20
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD 20)
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD_REQUIRED ON)
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_EXTENSIONS OFF)

if (WIN32)
	target_compile_definitions(${PROJECT_NAME} PRIVATE NOMINMAX) # stop windows.h conflicting with 'std::max'
endif()

# These are public as linking executable must also have these defined for some include files
if (GC_DEV_BUILD)
	target_compile_definitions(${PROJECT_NAME} PUBLIC GC_DO_ASSERTS)
	target_compile_definitions(${PROJECT_NAME} PUBLIC GC_LOG_TRACE_DEBUG)
	target_compile_definitions(${PROJECT_NAME} PUBLIC GC_LOOKUP_ASSET_IDS)
endif()
target_compile_definitions(${PROJECT_NAME} PUBLIC GC_LOGGER=GC_LOGGER_SPDLOG) # GC_LOGGER_SPDLOG or GC_LOGGER_NULL

# compiler warnings
if (MSVC)
	target_compile_options(${PROJECT_NAME} PRIVATE /W3)
	target_compile_options(${PROJECT_NAME} PRIVATE /MP)
else()
	target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -pedantic)
endif()

target_include_directories(${PROJECT_NAME} PUBLIC include)
target_include_directories(${PROJECT_NAME} PRIVATE src)

# spdlog
target_link_libraries(${PROJECT_NAME} PRIVATE spdlog::spdlog)