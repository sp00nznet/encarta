add_executable(test_vlc test_vlc.c lifted_manual.c)

if(MSVC)
    target_link_options(test_vlc PRIVATE /LARGEADDRESSAWARE:NO)
endif()
