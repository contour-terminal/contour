# Setup ccache.
#
# The ccache is auto-enabled if the tool is found.
# To disable set -DCCACHE=OFF option.
option(ENABLE_CCACHE "Enable compiler cache (ccache) [default: ON]" ON)
if(ENABLE_CCACHE)
    if(NOT DEFINED CMAKE_CXX_COMPILER_LAUNCHER)
        find_program(CCACHE ccache DOC "ccache tool path; set to OFF to disable")
        if(CCACHE)
            set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE})
            if(COMMAND cotire)
                # Change ccache config to meet cotire requirements.
                set(ENV{CCACHE_SLOPPINESS} pch_defines,time_macros)
            endif()
            message(STATUS "[ccache] Enabled: ${CCACHE}")
        else()
            message(STATUS "[ccache] Disabled: Not found.")
        endif()
    endif()
else()
    message(STATUS "[ccache] Disabled.")
endif()
