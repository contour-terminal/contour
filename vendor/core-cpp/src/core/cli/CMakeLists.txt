# SPDX-License-Identifier: Apache-2.0

# core::cli: the command-line parser and the application scaffold built on it.
core_cpp_add_module(cli KIND STATIC
    HEADERS App.hpp CLI.hpp
    SOURCES App.cpp CLI.cpp
    PUBLIC_LIBS core::base core::log)

core_cpp_add_test(cli SOURCES App_test.cpp CLI_test.cpp)
if(TEST core-cpp.cli)
    # The help renderer wraps text in a loop, and a wrapping loop that stops making progress does
    # not fail -- it runs until something kills it. One did (Task A9 fix round 1): without a bound
    # ctest waits out its own default, so a regression reads as a 25-minute silence rather than as
    # a failure. The suite itself takes hundredths of a second, so this is only ever a hang.
    set_tests_properties(core-cpp.cli PROPERTIES TIMEOUT 60)
endif()
