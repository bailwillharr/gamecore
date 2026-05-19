cmake_minimum_required(VERSION 3.25)

project(gctemplates LANGUAGES CXX)

set(INCLUDE_FILES
  "include/gctemplates/gct_sv_stream.h"
  "include/gctemplates/gct_static_vector.h"
  "include/gctemplates/gct_maybe_owning.h"
)

add_library(${PROJECT_NAME} INTERFACE
  ${INCLUDE_FILES}
)

target_include_directories(${PROJECT_NAME} INTERFACE include)

# This project uses C++20
target_compile_features(${PROJECT_NAME} INTERFACE cxx_std_20)
set_target_properties(${PROJECT_NAME} PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED YES
  CXX_EXTENSIONS NO
)

add_library(${PROJECT_NAME}_test STATIC "src/test.cpp")
target_link_libraries(${PROJECT_NAME}_test PRIVATE ${PROJECT_NAME})
