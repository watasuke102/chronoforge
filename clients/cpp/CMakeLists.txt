cmake_minimum_required(VERSION 3.5) # CMAKE_EXPORT_COMPILE_COMMANDS requires at least 3.5

project(cpp_client)

set(CMAKE_COLOR_DIAGNOSTICS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(${CMAKE_PROJECT_NAME}
  cpp_client.cpp
)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../runtime ${CMAKE_CURRENT_BINARY_DIR}/runtime)

target_link_libraries(${CMAKE_PROJECT_NAME}
  PRIVATE
    runtime::runtime
)
