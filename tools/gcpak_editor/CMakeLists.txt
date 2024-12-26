cmake_minimum_required(VERSION 3.24)

project(gcpak_editor LANGUAGES CXX
	VERSION "0.1.0"
)

# from command: find . -regex "^\.\/.*" | sort
# Header files can also be here if they're not part of the public API
set(SRC_FILES
	"src/main.cpp"
)

# gamecore is a static library
add_executable(${PROJECT_NAME}
	${SRC_FILES}
)

# compiling options:

if (WIN32)
	target_compile_definitions(${PROJECT_NAME} PRIVATE NOMINMAX) # stop windows.h conflicting with 'std::max'
endif()

# This project uses C++20
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD 20)
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD_REQUIRED ON)
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_EXTENSIONS OFF)

# compiler warnings
if (MSVC)
	target_compile_options(${PROJECT_NAME} PRIVATE /W4)
	target_compile_options(${PROJECT_NAME} PRIVATE /MP)
	#target_compile_definitions(${PROJECT_NAME} PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
	target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -pedantic)
endif()

target_include_directories(${PROJECT_NAME} PRIVATE src)