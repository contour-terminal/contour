# Test support every first-party suite may share. Nothing here is linked into a shipped binary.

# The case trace (CaseTrace.hpp): a suite links it to name, on standard error, each test case as it starts,
# when CONTOUR_TEST_TRACE is set. An INTERFACE library, so CaseTrace.cpp is compiled into the suite itself:
# a listener registers through a static initializer, which a linker would drop from a static library that
# nothing references.
add_library(contour_test_trace INTERFACE)
target_sources(contour_test_trace INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/CaseTrace.cpp)
target_include_directories(contour_test_trace INTERFACE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(contour_test_trace INTERFACE core::base Catch2::Catch2)

if(CONTOUR_TESTING)
    enable_testing()
    add_executable(testing_test test_main.cpp CaseTrace_test.cpp)
    target_link_libraries(testing_test contour_test_trace core::testing Catch2::Catch2)
    add_test(NAME testing_test COMMAND $<TARGET_FILE:testing_test>)

    if(NOT WIN32)
        # Catch2's TEST_CASE expands __COUNTER__, which Clang flags as a C2y extension under -Werror.
        target_compile_options(testing_test PRIVATE -Wno-c2y-extensions)
    endif()
endif()
