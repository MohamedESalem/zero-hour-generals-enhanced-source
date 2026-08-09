
# Print some information
message(STATUS "CMAKE_CXX_COMPILER: ${CMAKE_CXX_COMPILER}")
message(STATUS "CMAKE_CXX_COMPILER_ID: ${CMAKE_CXX_COMPILER_ID}")
message(STATUS "CMAKE_CXX_COMPILER_VERSION: ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "CMAKE_INSTALL_PREFIX: ${CMAKE_INSTALL_PREFIX}")
if (DEFINED MSVC_VERSION)
    message(STATUS "MSVC_VERSION: ${MSVC_VERSION}")
endif()

# TheSuperHackers @build JohnsterID 05/01/2026 Add MinGW-w64 detection and configure compiler flags
# Detect MinGW-w64
if(MINGW)
    message(STATUS "MinGW-w64 detected")
    set(IS_MINGW_BUILD TRUE)
else()
    set(IS_MINGW_BUILD FALSE)
endif()

# Set variable for VS6 to handle special cases.
if (DEFINED MSVC_VERSION AND MSVC_VERSION LESS 1300)
    set(IS_VS6_BUILD TRUE)
else()
    set(IS_VS6_BUILD FALSE)
endif()

# Make release builds have debug information too.
if(MSVC)
    # Create PDB for Release as long as debug info was generated during compile.
    string(APPEND CMAKE_EXE_LINKER_FLAGS_RELEASE " /DEBUG /OPT:REF /OPT:ICF")
    string(APPEND CMAKE_SHARED_LINKER_FLAGS_RELEASE " /DEBUG /OPT:REF /OPT:ICF")
    
    # /INCREMENTAL:NO prevents PDB size bloat in Debug configuration(s).
    add_link_options("/INCREMENTAL:NO")

    # Raise usable address space from 2GB to ~4GB on 64-bit Windows for ultra-large maps.
    # TheSuperHackers @fix mohamed 09/08/2026 Confirmed empirically that the VC6SP6 portable
    # toolchain's linker (link.exe) DOES accept /LARGEADDRESSAWARE and sets the corresponding
    # PE header characteristic correctly, despite the previous assumption that "VC6's linker
    # predates this flag." Without this, a large custom map (e.g. a 2048x2048 world map, tens
    # of MB of heightmap data plus this build's asset set) reliably exhausts the 2GB default
    # address space and the engine throws ERROR_OUT_OF_MEMORY (Common/Errors.h) shortly after
    # a match starts. Verified: PE header LARGE_ADDRESS_AWARE characteristic bit set post-link.
    add_link_options("/LARGEADDRESSAWARE")

    # TheSuperHackers @fix mohamed 10/08/2026 Modest stack bump for ultra-large maps.
    # IMPORTANT: this value is the PE header's SizeOfStackReserve, which Windows uses as the
    # default reserve for EVERY thread created without an explicit stack size -- not just the
    # main thread. The game runs ~16 threads, so this is multiplied by ~16 in reserved address
    # space. That matters enormously in a 32-bit process capped at ~4GB: an earlier 64MB value
    # (used while diagnosing a suspected stack overflow) silently reserved ~1GB of address
    # space for nothing. The real bug turned out to be the fixed-size zone tables in
    # PathfindZoneManager::calculateZones, which now live on the heap, so the deep stack is
    # no longer needed. 4MB leaves generous headroom over the 1MB default at ~64MB total cost.
    add_link_options("/STACK:4194304")
else()
    # We go a bit wild here and assume any other compiler we are going to use supports -g for debug info.
    # Add debug symbols to Release builds for crash dump analysis, profiling, and post-mortem debugging.
    # For MinGW, symbols will be stripped to separate .debug files (matching MSVC PDB workflow).
    string(APPEND CMAKE_CXX_FLAGS_RELEASE " -g")
    string(APPEND CMAKE_C_FLAGS_RELEASE " -g")

    if(MINGW)
        # MinGW-w64 equivalent of MSVC's /LARGEADDRESSAWARE.
        add_link_options("-Wl,--large-address-aware")
    endif()
endif()

set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)  # Ensures only ISO features are used

if (NOT IS_VS6_BUILD)
    if (MSVC)
        # Multithreaded build.
        add_compile_options(/MP)
        # Enforce strict __cplusplus version
        add_compile_options(/Zc:__cplusplus)
    else()
        add_compile_options(-Wsuggest-override)
    endif()
else()
    if(RTS_BUILD_OPTION_VC6_FULL_DEBUG)
        set_property(GLOBAL PROPERTY JOB_POOLS compile=1 link=1)
    else()
        # Define two pools: 'compile' with plenty of slots, 'link' with just one
        set_property(GLOBAL PROPERTY JOB_POOLS compile=0 link=1)
    endif()

    # Tell CMake that all compile steps go into 'compile'
    set(CMAKE_JOB_POOL_COMPILE compile)
    # and all link steps go into 'link' (so only one link ever runs since vc6 can't handle multithreaded linking)
    set(CMAKE_JOB_POOL_LINK link)
endif()

if(RTS_BUILD_OPTION_ASAN)
    if(MSVC)
        set(ENV{ASAN_OPTIONS} "shadow_scale=2")
        add_compile_options(/fsanitize=address)
        add_link_options(/fsanitize=address)
    else()
        add_compile_options(-fsanitize=address)
        add_link_options(-fsanitize=address)
    endif()
endif()
