cmake_minimum_required(VERSION 3.24)

project(gamecore_test LANGUAGES CXX)

# test executable for debugging
add_executable(${PROJECT_NAME}
  "src/main.cpp"
)

# This project uses C++20
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD 20)
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD_REQUIRED ON)
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_EXTENSIONS OFF)

# compiler warnings
if (MSVC)
	target_compile_options(${PROJECT_NAME} PRIVATE /W3)
	target_compile_options(${PROJECT_NAME} PRIVATE /MP)
else()
	target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -pedantic)
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE gamecore)

# Link content directory to the output binary directory
add_custom_command(
    TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E create_symlink
            "${PROJECT_SOURCE_DIR}/content"
            "${PROJECT_BINARY_DIR}/content"
    COMMENT "Linking content directory to the output binary directory"
)
