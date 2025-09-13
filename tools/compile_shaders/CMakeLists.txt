cmake_minimum_required(VERSION 3.25)

project(compile_shaders LANGUAGES CXX)

set(SRC_FILES
	"src/main.cpp"
)

add_executable(${PROJECT_NAME}
	${SRC_FILES}
)

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