cmake_minimum_required(VERSION 3.25)

project(dedicated_server LANGUAGES CXX)

set(SRC_FILES
	"src/main.cpp"
)

add_executable(${PROJECT_NAME} ${SRC_FILES})

# compiling options:

if (MSVC)
	target_compile_options(${PROJECT_NAME} PRIVATE /W4 /WX) # warning level 4 and treat warnings as errors
endif()

if (WIN32)
	target_compile_definitions(${PROJECT_NAME} PRIVATE NOMINMAX) # stop windows.h conflicting with 'std::max'
endif()

# This project uses C++20
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD 20)
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD_REQUIRED ON)
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_EXTENSIONS OFF)

target_link_libraries(${PROJECT_NAME} PRIVATE gamecore)

if(WIN32)
    add_custom_command(
        TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy $<TARGET_FILE:SDL3::SDL3-shared> $<TARGET_FILE_DIR:${PROJECT_NAME}>
        VERBATIM
    )
endif()

# link game content directory to output directory
add_custom_command(
	TARGET ${PROJECT_NAME} POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E create_symlink ${PROJECT_SOURCE_DIR}/../content ${PROJECT_BINARY_DIR}/content
	VERBATIM
)