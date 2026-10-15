cmake_minimum_required(VERSION 3.20)
project(encarta-tools LANGUAGES C)

set(CMAKE_C_STANDARD 17)

# MSVC-specific settings (clang-cl counts as MSVC, so the cross build gets them too)
if(MSVC)
    add_compile_options(/W4 /utf-8)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
endif()

# The data tools were written against the MSVC CRT; these are its only spellings
# that POSIX lacks.
if(NOT WIN32)
    add_compile_definitions(_stricmp=strcasecmp _strnicmp=strncasecmp)
endif()

add_subdirectory(tools)
