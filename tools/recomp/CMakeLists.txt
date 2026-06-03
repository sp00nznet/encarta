add_executable(test_vlc test_vlc.c lifted_manual.c cpu_rt.c)
add_executable(test_vlc_auto test_vlc.c lifted_auto.c cpu_rt.c)   # same harness, generated lift
add_executable(recomp_decode recomp_decode.c lifted_codec.c lifted_setup.c cpu_rt.c)  # full lifted pipeline

if(MSVC)
    target_link_options(test_vlc PRIVATE /LARGEADDRESSAWARE:NO)
    target_link_options(test_vlc_auto PRIVATE /LARGEADDRESSAWARE:NO)
    target_link_options(recomp_decode PRIVATE /LARGEADDRESSAWARE:NO)
    target_compile_options(recomp_decode PRIVATE /wd4244 /wd4146)  # generated-code narrowing/unary-minus
endif()
target_include_directories(recomp_decode PRIVATE ${CMAKE_SOURCE_DIR}/tools/decooracle)
