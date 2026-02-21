add_executable(ftcdecode ftcdecode.c)

# Math library on non-MSVC
if(NOT MSVC)
    target_link_libraries(ftcdecode m)
endif()
