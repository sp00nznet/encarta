# The data tools are plain C and build anywhere. The rest drive Win32 code
# (the original DLLs, or the recompiled app against real MFC) and need a 32-bit
# Windows target: MSVC, or clang-cl from Linux/macOS (cmake/clang-cl-x86.cmake).
add_subdirectory(strdump)
add_subdirectory(m20dump)
add_subdirectory(datdump)
add_subdirectory(ftcdecode)
add_subdirectory(indeo)

if(WIN32)
    add_subdirectory(fifdecode)
    add_subdirectory(decooracle)
    add_subdirectory(spamdump)
    add_subdirectory(recomp)
endif()
