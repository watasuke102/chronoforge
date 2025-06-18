cmake_minimum_required(VERSION 3.5) # CMAKE_EXPORT_COMPILE_COMMANDS requires at least 3.5

project(scheduler)

set(CMAKE_COLOR_DIAGNOSTICS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(scheduler
  main.cpp
)

target_include_directories(scheduler
  PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../common
)
