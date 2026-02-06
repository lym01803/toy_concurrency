cmake_minimum_required(VERSION 3.21)

project(toy_concurrency)

add_library(toy_concurrency INTERFACE)
target_include_directories(toy_concurrency INTERFACE 
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_compile_features(toy_concurrency INTERFACE cxx_std_20)

if(PROJECT_IS_TOP_LEVEL)
    add_executable(toy_concurrency_example examples/example.cpp)
    target_link_libraries(toy_concurrency_example PRIVATE toy_concurrency)
endif()
