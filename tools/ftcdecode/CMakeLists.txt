add_executable(ftcdecode ftcdecode.c)
add_executable(ftcprobe ftcprobe.c)

# Math library on non-MSVC
if(NOT MSVC)
    target_link_libraries(ftcdecode m)
    target_link_libraries(ftcprobe m)
endif()
