add_executable(decooracle decooracle.c)
add_executable(decotrace decotrace.c)

# 32-bit host so it can drive the PE32 DECO_32.DLL under WOW64.
if(MSVC)
    target_link_options(decooracle PRIVATE /LARGEADDRESSAWARE:NO)
    target_link_options(decotrace  PRIVATE /LARGEADDRESSAWARE:NO)
endif()
