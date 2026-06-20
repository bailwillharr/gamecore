cmake_minimum_required(VERSION 3.25)

option(GC_LOG_TRACE "logs GC_TRACE calls (very verbose)" OFF)
option(GC_DISABLE_LOGGING "Disable logging completely" OFF)

project(gclog LANGUAGES CXX VERSION "0.1.0")

set(SRC_FILES
  "src/gclog.cpp"
  "src/gclog_spdlog.cpp"
  "src/gclog_spdlog.h"
)

set(INCLUDE_FILES
  "include/gclog/gclog.h"
)

add_library(${PROJECT_NAME} STATIC
  ${SRC_FILES}
  ${INCLUDE_FILES}
)

# This project uses C++20
target_compile_features(${PROJECT_NAME} PUBLIC cxx_std_20)
set_target_properties(${PROJECT_NAME} PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED YES
  CXX_EXTENSIONS NO
)

if(GC_DEV_BUILD)
    target_compile_definitions(${PROJECT_NAME} PUBLIC GC_LOG_DEBUG)
endif(GC_DEV_BUILD)
if (GC_LOG_TRACE)
    target_compile_definitions(${PROJECT_NAME} PUBLIC GC_LOG_TRACE)
endif(GC_LOG_TRACE)
if (GC_DISABLE_LOGGING)
    target_compile_definitions(${PROJECT_NAME} PUBLIC GC_LOGGER=GC_LOGGER_NULL)
else()
    target_compile_definitions(${PROJECT_NAME} PUBLIC GC_LOGGER=GC_LOGGER_SPDLOG)
endif()

target_include_directories(${PROJECT_NAME} PUBLIC include)
target_include_directories(${PROJECT_NAME} PRIVATE src)

target_link_libraries(${PROJECT_NAME} PRIVATE spdlog::spdlog)
