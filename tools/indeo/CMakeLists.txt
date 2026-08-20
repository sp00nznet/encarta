add_executable(indeodec indeodec.c indeo3.c)
if(MSVC)
    target_compile_options(indeodec PRIVATE /W3)
endif()
