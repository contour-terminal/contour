# Setup ccache/sccache.
#
# The compiler cache is auto-enabled if the tool is found.
# To disable set -DENABLE_CCACHE=OFF option.
option(ENABLE_CCACHE "Enable compiler cache (ccache/sccache) [default: ON]" ON)
if(ENABLE_CCACHE)
    if(NOT DEFINED CMAKE_CXX_COMPILER_LAUNCHER)
        # Try sccache first (better Windows/MSVC support), then fall back to ccache
        find_program(CCACHE sccache DOC "sccache tool path")
        if(NOT CCACHE)
            find_program(CCACHE ccache DOC "ccache tool path")
        endif()

        if(CCACHE)
            set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE})
            set(CMAKE_C_COMPILER_LAUNCHER ${CCACHE})
            if(COMMAND cotire)
                # Change ccache config to meet cotire requirements.
                set(ENV{CCACHE_SLOPPINESS} pch_defines,time_macros)
            endif()
            # ccache/sccache cannot cache MSVC compilations that use /Zi
            # (separate .pdb): the .pdb is a side effect written by a separate
            # process the cache neither captures nor serializes, so concurrent
            # /MP invocations collide on the shared vc140.pdb (fatal error
            # C1041). Embed debug info in each .obj via /Z7 instead, which is
            # cacheable and pdb-free. Applies to both cl and ClangCL (MSVC set).
            #
            # CMAKE_MSVC_DEBUG_INFORMATION_FORMAT only takes effect under policy
            # CMP0141=NEW. Our own minimum (3.25) sets it NEW, but third-party
            # subprojects fetched via CPM (e.g. libunicode declares
            # cmake_minimum_required 3.14) reset the policy in their own scope
            # and would otherwise fall back to /Zi. CMAKE_POLICY_DEFAULT_CMP0141
            # forces NEW into those subprojects too.
            if(MSVC)
                set(CMAKE_POLICY_DEFAULT_CMP0141 NEW)
                set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "Embedded")
                message(STATUS "[ccache] Using /Z7 (embedded debug info) for cacheable MSVC builds")
            endif()
            message(STATUS "[ccache] Enabled: ${CCACHE}")
        else()
            message(STATUS "[ccache] Disabled: Not found.")
        endif()
    else()
        # Compiler launcher already set (e.g., by CI environment)
        message(STATUS "[ccache] Using pre-configured launcher: ${CMAKE_CXX_COMPILER_LAUNCHER}")
    endif()
else()
    message(STATUS "[ccache] Disabled.")
endif()
