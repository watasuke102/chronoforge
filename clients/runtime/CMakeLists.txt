cmake_minimum_required(VERSION 3.5) # CMAKE_EXPORT_COMPILE_COMMANDS requires at least 3.5

project(runtime LANGUAGES CXX C)

set(CMAKE_COLOR_DIAGNOSTICS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(runtime STATIC
	runtime.cpp
)
add_library(runtime::runtime ALIAS runtime)

target_include_directories(runtime
	PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../common
)

set_target_properties(runtime PROPERTIES POSITION_INDEPENDENT_CODE ON)
