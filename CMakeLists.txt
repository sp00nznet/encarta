cmake_minimum_required(VERSION 3.20)
project(encarta-tools LANGUAGES C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 20)

# MSVC-specific settings
if(MSVC)
    add_compile_options(/W4 /utf-8)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
endif()

add_subdirectory(tools)
