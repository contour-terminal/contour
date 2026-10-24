if(("${CMAKE_CXX_COMPILER_ID}" MATCHES "GNU") OR ("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang"))
    #(TODO: first get rid of these warnings) add_compile_options(-Werror)
endif()

include(PedanticCompiler)

# Coverage instruments the first-party code under src/ only, never the fetched third-party code. Instrumented,
# a library's translation units built for a CPU extension -- libunicode compiles its scanner for AVX-512 -- reset
# their counters with that extension's instructions, and the runtime resets every unit's counters in each fork
# child (__gcov_fork): on a CPU without the extension every child died of SIGILL before it could exec.
if(CONTOUR_CODE_COVERAGE_ENABLED)
    add_compile_options(-g --coverage)
endif()

add_subdirectory(crispy)
add_subdirectory(text_shaper)
add_subdirectory(vtpty)
add_subdirectory(vtcontrol)
add_subdirectory(vtparser)
add_subdirectory(vtbackend)
if(CONTOUR_EXTENSIONS)
    add_subdirectory(vtextension)
endif()
add_subdirectory(vtconformance)
add_subdirectory(vtworkspace)
add_subdirectory(vthost)
add_subdirectory(vtrasterizer)

add_subdirectory(contour)
