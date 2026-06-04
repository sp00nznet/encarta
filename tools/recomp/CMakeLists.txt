add_executable(test_vlc test_vlc.c lifted_manual.c cpu_rt.c)
add_executable(test_vlc_auto test_vlc.c lifted_auto.c cpu_rt.c)   # same harness, generated lift
add_executable(recomp_decode recomp_decode.c lifted_codec.c cpu_rt.c)  # full lifted pipeline (110 fns)
add_executable(recomp_encapi recomp_encapi.c encapi_lifted.c cpu_rt.c)  # ENCAPI32 lift + Win32 import trampoline
add_executable(recomp_enc97 recomp_enc97.c enc97_one.c cpu_rt.c)  # execute a lifted ENC97.EXE function

if(MSVC)
    target_link_options(test_vlc PRIVATE /LARGEADDRESSAWARE:NO)
    target_link_options(test_vlc_auto PRIVATE /LARGEADDRESSAWARE:NO)
    target_link_options(recomp_decode PRIVATE /LARGEADDRESSAWARE:NO)
    # keep mapped image + emulated stack in the low 32-bit range (CPU regs are uint32_t)
    target_link_options(recomp_enc97 PRIVATE /LARGEADDRESSAWARE:NO)
    target_compile_options(recomp_decode PRIVATE /wd4244 /wd4146)  # generated-code narrowing/unary-minus
endif()
target_include_directories(recomp_decode PRIVATE ${CMAKE_SOURCE_DIR}/tools/decooracle)
