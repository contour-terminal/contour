
option(ENABLE_TIDY "Enable clang-tidy [default: OFF]" OFF)
if(ENABLE_TIDY)
    find_program(CLANG_TIDY_EXE
        NAMES clang-tidy-8 clang-tidy-7 clang-tidy-6.0 clang-tidy
        DOC "Path to clang-tidy executable")
    if(NOT CLANG_TIDY_EXE)
        message(STATUS "clang-tidy not found.")
    else()
        message(STATUS "clang-tidy found: ${CLANG_TIDY_EXE}")
        set(DO_CLANG_TIDY "${CLANG_TIDY_EXE}")
    endif()
endif()
