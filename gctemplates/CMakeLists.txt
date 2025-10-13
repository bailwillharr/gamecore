cmake_minimum_required(VERSION 3.25)

project(gctemplates LANGUAGES CXX)

set(INCLUDE_FILES
  "include/gctemplates/gct_sv_stream.h"
  "include/gctemplates/gct_static_vector.h"
)

add_library(${PROJECT_NAME} INTERFACE
  ${INCLUDE_FILES}
)

target_include_directories(${PROJECT_NAME} INTERFACE include)

# This project uses C++20
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD 20)
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD_REQUIRED ON)
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_EXTENSIONS OFF)
