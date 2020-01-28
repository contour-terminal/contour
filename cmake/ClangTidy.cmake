
option(ENABLE_TIDY "Enable clang-tidy [default: OFF]" OFF)
if(ENABLE_TIDY)
    find_program(CLANG_TIDY_EXE
        NAMES clang-tidy-9 clang-tidy-8 clang-tidy-7 clang-tidy
        DOC "Path to clang-tidy executable")
    if(NOT CLANG_TIDY_EXE)
        message(STATUS "[clang-tidy] Not found.")
    else()
        message(STATUS "[clang-tidy] found: ${CLANG_TIDY_EXE}")
        set(CMAKE_CXX_CLANG_TIDY "${CLANG_TIDY_EXE}")
    endif()
else()
    message(STATUS "[clang-tidy] Disabled.")
endif()
