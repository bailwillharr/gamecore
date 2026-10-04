cmake_minimum_required(VERSION 3.25)

project(prefab_test LANGUAGES CXX)

set(SRC_FILES
  "src/main.cpp"
)

# always a console application so that results are printed
add_executable(${PROJECT_NAME}
  ${SRC_FILES}
)

# compiling options:

if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4 /WX)
endif()

if(WIN32)
    # stop windows.h conflicting with 'std::max'
    target_compile_definitions(${PROJECT_NAME} PRIVATE NOMINMAX)
endif()

# This project uses C++20
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_20)
set_target_properties(${PROJECT_NAME} PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED YES
  CXX_EXTENSIONS NO
)

target_link_libraries(${PROJECT_NAME} PRIVATE gamecore)

if(WIN32)
    add_custom_command(
        TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy
        $<TARGET_FILE:SDL3::SDL3-shared> $<TARGET_FILE_DIR:${PROJECT_NAME}>
        VERBATIM
    )
endif()
