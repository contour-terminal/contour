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
