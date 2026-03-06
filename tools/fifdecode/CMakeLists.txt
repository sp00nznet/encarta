add_executable(fifdecode fifdecode.c)

# Disable DEP for compatibility with 1995-era DECO_32.DLL
# (the DLL may execute code from data pages)
if(MSVC)
    target_link_options(fifdecode PRIVATE /NXCOMPAT:NO /LARGEADDRESSAWARE)
endif()
