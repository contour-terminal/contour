# The VT conformance harness.
#
# Drives an external VT test program (vttest today) against Contour's own terminal engine over a real
# PTY, entirely headlessly, and judges the result with a set of oracles -- three of which need no
# golden files at all. Qt-free by construction: it sits above vtbackend and below contour.

add_library(vtconformance STATIC
    Diagnostics.cpp
    EscTestLog.cpp
    EscTestSuite.cpp
    Diagnostics.h
    MarkerScanner.cpp
    MarkerScanner.h
    Report.cpp
    Report.h
    Runner.cpp
    Runner.h
    ScreenDump.cpp
    ScreenDump.h
    Suite.h
    TerminalEngine.cpp
    TerminalEngine.h
    TerminalHarness.cpp
    TerminalHarness.h
    VtTestLog.cpp
    VtTestLog.h
    VtTestSuite.cpp
)
set_target_properties(vtconformance PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_EXE}")
target_link_libraries(vtconformance PUBLIC
    vtbackend
    vtpty
    crispy::core
)
target_include_directories(vtconformance PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/include>
)

add_executable(vtconformance-run main.cpp)
set_target_properties(vtconformance-run PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_EXE}")
target_link_libraries(vtconformance-run PRIVATE vtconformance)

option(VTCONFORMANCE_TESTING "Enables building of unittests for vtconformance [default: ON]" ${CONTOUR_TESTING})
if(VTCONFORMANCE_TESTING)
    enable_testing()
    add_executable(vtconformance_test
        test_main.cpp
        Diagnostics_test.cpp
        EscTestLog_test.cpp
        MarkerScanner_test.cpp
        Report_test.cpp
        ScreenDump_test.cpp
        TerminalEngine_test.cpp
        TerminalHarness_test.cpp
        VtTestLog_test.cpp
    )
    target_link_libraries(vtconformance_test vtconformance Catch2::Catch2)
    add_test(NAME vtconformance_test COMMAND $<TARGET_FILE:vtconformance_test>)

    if(NOT WIN32)
        # Catch2's TEST_CASE expands __COUNTER__, which Clang flags as a C2y extension under -Werror.
        target_compile_options(vtconformance_test PRIVATE -Wno-c2y-extensions)
    endif()

    # The end-to-end conformance gate.
    #
    # Gating is decided per SCENARIO, not per suite (see Scenario::gatesBuild). vttest's self-checking
    # chapters are driven by replaying a recorded command file; its visual chapters are driven by the
    # markers vttest flushes on the wire and judged against goldens. Both are captured at a causal
    # barrier rather than on a timeout, so both are reproducible and both break the build. Note that many
    # of those goldens are recorded rather than reviewed: they pin today's behaviour without asserting it
    # is correct, which a gate serves perfectly well -- it makes a change to them deliberate. What keeps
    # this honest rather than merely noisy is the ratchet in known-gaps.txt -- see Report.h.
    #
    # vttest is not a build dependency, so a machine without it skips rather than fails. A skip says
    # so loudly on stderr; it never masquerades as a pass.
    if(NOT WIN32)
        add_test(NAME vtconformance_vttest
            COMMAND $<TARGET_FILE:vtconformance-run> run
                    --suite vttest
                    --skip-if-missing
                    --golden-dir "${CMAKE_CURRENT_SOURCE_DIR}/test/golden"
                    --known-gaps "${CMAKE_CURRENT_SOURCE_DIR}/test/known-gaps.txt"
                    --command-dir "${CMAKE_CURRENT_SOURCE_DIR}/test/cmd"
                    --work-dir "${CMAKE_CURRENT_BINARY_DIR}/conformance-work"
        )
        # Generous: a replayed scenario pays vttest's hard-coded 1s sleep per answer it reads from the
        # command file (unix_io.c:190,252), plus 5s apiece for the RIS and DECTST settling waits
        # (reset.c:37,56). Determinism is worth the wall-clock; guessing was faster and wrong.
        set_tests_properties(vtconformance_vttest PROPERTIES
            TIMEOUT 1800
            RUN_SERIAL TRUE
            LABELS conformance
        )

        # The esctest gate.
        #
        # esctest asserts rather than draws: it reads the screen back through DECRQCRA and checks
        # every cell against what it expects, so it needs no goldens and no human. That is what makes
        # it worth far more per test than vttest -- and it is why DECRQCRA had to be correct first.
        #
        # It is ratcheted exactly as vttest is, on two axes: the sequences the engine ignores, and the
        # tests that fail. A failure that is not listed breaks the build; a listed failure that now
        # passes ALSO breaks it. So the list can only shrink, and it cannot quietly stop describing
        # reality.
        #
        # esctest is GPL-2.0 and Contour is Apache-2.0: fetch it with scripts/fetch-esctest.sh; it is
        # invoked as an external program, never vendored and never linked. Absent, this skips loudly
        # rather than failing -- "you did not clone a GPL repository" is not a conformance regression.
        add_test(NAME vtconformance_esctest
            COMMAND $<TARGET_FILE:vtconformance-run> run
                    --suite esctest
                    --skip-if-missing
                    --suite-dir "${PROJECT_SOURCE_DIR}/out/esctest2/esctest"
                    --known-gaps "${CMAKE_CURRENT_SOURCE_DIR}/test/esctest-known-gaps.txt"
                    --known-failures "${CMAKE_CURRENT_SOURCE_DIR}/test/esctest-known-failures.txt"
                    --work-dir "${CMAKE_CURRENT_BINARY_DIR}/esctest-work"
        )
        set_tests_properties(vtconformance_esctest PROPERTIES
            TIMEOUT 1800
            RUN_SERIAL TRUE
            LABELS conformance
        )
    endif()
endif()
