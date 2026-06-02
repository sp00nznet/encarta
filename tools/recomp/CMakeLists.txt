add_executable(test_vlc test_vlc.c lifted_manual.c cpu_rt.c)
add_executable(test_vlc_auto test_vlc.c lifted_auto.c cpu_rt.c)   # same harness, generated lift

if(MSVC)
    target_link_options(test_vlc PRIVATE /LARGEADDRESSAWARE:NO)
    target_link_options(test_vlc_auto PRIVATE /LARGEADDRESSAWARE:NO)
endif()
