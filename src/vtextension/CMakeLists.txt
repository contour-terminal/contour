# vtextension: hosts WebAssembly status-line extensions on a runtime-neutral interface (wasm/WasmRuntime.hpp).
# Qt-free, so that both the GUI (src/contour) and the daemon (src/vthost) can link it. It links no WebAssembly
# runtime: the backend selected by CONTOUR_WASM_RUNTIME is its own target (wasm/<runtime>/), and a program
# links it through vtextension_runtime, defined at the bottom.

set(vtextension_SOURCES
    BundledExtensions.cpp
    CanonicalAbi.cpp
    ChildEnvironment.cpp
    CommandLine.cpp
    CommandPolicy.cpp
    CompiledExtension.cpp
    ControlImports.cpp
    DaemonExtensionRunner.cpp
    DaemonExtensionRuntime.cpp
    DaemonImports.cpp
    DaemonInstance.cpp
    DeclaredArguments.cpp
    DeclaredPanes.cpp
    DeclaredProcesses.cpp
    Engine.cpp
    EventQueue.cpp
    ExtensionComposition.cpp
    ExtensionConsent.cpp
    ExtensionGrantStore.cpp
    ExtensionHost.cpp
    ExtensionWorkerPool.cpp
    FileExtensionStore.cpp
    FileReadService.cpp
    FileLocker_unix.cpp
    FileLocker_win32.cpp
    GuestCall.cpp
    HostBindings.cpp
    HostState.cpp
    Instance.cpp
    Manifest.cpp
    RootPointer.cpp
    RootResolution.cpp
    Runner.cpp
    SelfPrograms.cpp
    SessionExtensions.cpp
    SettableEnvironment.cpp
    StatusLineAdapter.cpp
    Surface.cpp
)

set(vtextension_HEADERS
    ArgumentKind.hpp
    AtomicSharedPtr.hpp
    BundledExtensions.hpp
    CanonicalAbi.hpp
    ChildEnvironment.hpp
    CommandLine.hpp
    CommandPolicy.hpp
    CompiledExtension.hpp
    ControlCodec.hpp
    ControlImports.hpp
    DaemonExtensionRunner.hpp
    DaemonExtensionRuntime.hpp
    DaemonImports.hpp
    DaemonInstance.hpp
    DaemonLimits.hpp
    DaemonWorld.hpp
    DeadlineGuard.hpp
    DeclaredArguments.hpp
    DeclaredPanes.hpp
    DeclaredProcesses.hpp
    Engine.hpp
    Event.hpp
    EventQueue.hpp
    ExtensionAutostart.hpp
    ExtensionComposition.hpp
    ExtensionConsent.hpp
    ExtensionError.hpp
    ExtensionGrantStore.hpp
    ExtensionHost.hpp
    ExtensionLimits.hpp
    ExtensionPresence.hpp
    ExtensionScheduler.hpp
    ExtensionStore.hpp
    ExtensionWorkerPool.hpp
    FileExtensionStore.hpp
    FileReadService.hpp
    FileLocker.hpp
    GuestCall.hpp
    HostBindings.hpp
    HostState.hpp
    IDaemonHostServices.hpp
    IHostServices.hpp
    Instance.hpp
    Logging.hpp
    Manifest.hpp
    PaneService.hpp
    Placement.hpp
    ProcessService.hpp
    RootPointer.hpp
    RootResolution.hpp
    Runner.hpp
    SelfPrograms.hpp
    SettableEnvironment.hpp
    ServedTabService.hpp
    SessionExtensions.hpp
    StatusLineAdapter.hpp
    StatuslineWorld.hpp
    Surface.hpp
    Utf8.hpp
    World.hpp
    wasm/DefaultBackend.hpp
    wasm/WasiAllowList.hpp
    wasm/WasmBinary.hpp
    wasm/WasmRuntime.hpp
)

add_library(vtextension STATIC ${vtextension_SOURCES} ${vtextension_HEADERS})
target_compile_definitions(vtextension PUBLIC CONTOUR_EXTENSIONS=1)
target_include_directories(vtextension PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/include>
)
# vtpty for PageSize, the render call's surface size, and the CommandCapture extensions run commands through;
# vtbackend for the Surface's private terminal; core::async for the thread a session's modules are loaded on.
target_link_libraries(vtextension PUBLIC vtbackend vtcontrol vtpty core::async core::base core::log core::platform)
# Private, as in vtcontrol: ControlCodec.hpp reflects the control vocabulary through glaze, and it is included
# from vtextension's .cpp files only. vtextension_public_headers_test below pins that no public header leaks it.
target_link_libraries(vtextension PRIVATE glaze::glaze)

option(VTEXTENSION_TESTING "Enables building of unittests for vtextension [default: ON]" ${CONTOUR_TESTING})
if(VTEXTENSION_TESTING)
    enable_testing()

    add_executable(vtextension_test
        test_main.cpp
        ArgumentKind_test.cpp
        Bundled_test.cpp
        BundledExtensions_test.cpp
        CanonicalAbi_test.cpp
        ChildEnvironment_test.cpp
        CommandLine_test.cpp
        CommandPolicy_test.cpp
        ControlCodec_test.cpp
        ControlImports_test.cpp
        DaemonExtensionRunner_test.cpp
        DaemonExtensionRuntime_test.cpp
        DaemonGuest_test.cpp
        DaemonGuestModule_test.cpp
        DaemonInstance_test.cpp
        DaemonWorld_test.cpp
        DeadlineGuard_test.cpp
        DeclaredCleanup_test.cpp
        DeclaredPanes_test.cpp
        DeclaredProcesses_test.cpp
        Engine_test.cpp
        EventQueue_test.cpp
        ExtensionComposition_test.cpp
        ExtensionConsent_test.cpp
        ExtensionGrantStore_test.cpp
        ExtensionHost_test.cpp
        ExtensionWorkerPool_test.cpp
        FileExtensionStore_test.cpp
        FileReadService_test.cpp
        HostState_test.cpp
        Instance_test.cpp
        Isolation_test.cpp
        Manifest_test.cpp
        ParseBudget_test.cpp
        RootPointer_test.cpp
        RootResolution_test.cpp
        Runner_test.cpp
        SessionExtensions_test.cpp
        SettableEnvironment_test.cpp
        StatusLineAdapter_test.cpp
        Surface_test.cpp
        Wit_test.cpp
        WorldCheck_test.cpp
        wasm/WasmBinary_test.cpp
        testing/DaemonRunnerHarness.hpp
        testing/FailingFileSystem.hpp
        testing/FakeBeneathReader.hpp
        testing/FakeDaemonHostServices.hpp
        testing/FakeFileLocker.hpp
        testing/FakeFileReadService.hpp
        testing/FakePaneService.hpp
        testing/FakeDirectoryOpener.hpp
        testing/FakeGrantStore.hpp
        testing/FakeGuestMemory.hpp
        testing/FakeProcessRunner.hpp
        testing/FakeServedTabService.hpp
        testing/FakeWasmBackend.hpp
        testing/FixtureProcessRunner.hpp
        testing/GitSandbox.hpp
        testing/GuestModuleCheck.hpp
        testing/GuestSlowdown.hpp
        testing/Harness.hpp
        testing/LaneReport.hpp
        testing/OrchestratorManifest.hpp
        testing/ManualScheduler.hpp
        testing/QueuedOwner.hpp
        testing/RunnerHarness.hpp
        testing/StandardStreamsCapture.hpp
        testing/TestModules.hpp
        wasm/testing/BackendContract.hpp
        wasm/testing/PreparedGeneration.hpp
    )
    # The WIT package is the normative spec of the extension ABI; the tests read it from the source tree.
    target_compile_definitions(vtextension_test PRIVATE VTEXTENSION_WIT_DIR="${PROJECT_SOURCE_DIR}/wit/statusline")
    target_compile_definitions(vtextension_test PRIVATE VTEXTENSION_DAEMON_WIT_DIR="${PROJECT_SOURCE_DIR}/wit/daemon")
    # Manifest version 2 is checked against the conformance corpus the orchestrator extension tests against too.
    target_compile_definitions(vtextension_test PRIVATE
        VTEXTENSION_MANIFEST_CORPUS_DIR="${PROJECT_SOURCE_DIR}/extensions/orchestrator/contract/manifest-v2")
    # The golden texts the tests compare against.
    target_compile_definitions(vtextension_test PRIVATE VTEXTENSION_TESTDATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/testdata")
    if(CONTOUR_WASM_GUESTS)
        # The test cases that run a Rust guest exist only when the guests can be built.
        foreach(_guest IN ITEMS echo hostile sleeper spawner)
            contour_add_wasm_module(NAME ${_guest} CRATE ${PROJECT_SOURCE_DIR}/extensions/testing/${_guest} OUTPUT_VAR _guest_wasm)
            add_dependencies(vtextension_test ${_guest}_wasm)
        endforeach()
        # The bundled extensions are built into the same directory, and tested from there.
        add_dependencies(vtextension_test contour_bundled_extensions)
        get_filename_component(VTEXTENSION_TEST_MODULE_DIR "${_guest_wasm}" DIRECTORY)
        target_compile_definitions(vtextension_test PRIVATE VTEXTENSION_TEST_MODULE_DIR="${VTEXTENSION_TEST_MODULE_DIR}")
        if(TARGET bench-headless)
            # The throughput benchmark with a well-behaved and a spinning extension beside its terminal: a crash
            # in its extension rig fails the suite. What it measures is reported, not asserted.
            add_test(NAME bench_headless_throughput_extensions_smoke
                     COMMAND bench-headless throughput size 1 runs 1 kinds ascii screens primary indicator
                             extensions "${VTEXTENSION_TEST_MODULE_DIR}/echo.wasm,${VTEXTENSION_TEST_MODULE_DIR}/hostile.wasm"
                             extension-config mode=spin)
            if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
                add_test(NAME bench_headless_memory_extensions_smoke
                         COMMAND bench-headless memory panes 2 seconds 0
                                 extensions "${VTEXTENSION_TEST_MODULE_DIR}/echo.wasm")
            endif()
        endif()
    else()
        message(STATUS "[vtextension] CONTOUR_EXTENSIONS_BUNDLED is OFF: the Rust-guest test cases are compiled out, and SKIP.")
    endif()
    if(CONTOUR_EXTENSIONS_ORCHESTRATOR_GUEST)
        # The C++ daemon-world probe, built with the WASI C++ toolchain the orchestrator's guest is built with:
        # checked as a module and run on the daemon engine.
        contour_add_wasm_cxx_guest(NAME cxx-daemon-probe
                                   SOURCE_DIR ${PROJECT_SOURCE_DIR}/extensions/testing/cxx-daemon-probe
                                   OUTPUT_VAR _cxx_probe_wasm
                                   # A list value, which the probe's configuration checks arrives whole.
                                   CMAKE_ARGS "-DCXX_PROBE_EXPECTED_DIRECTORIES=${PROJECT_SOURCE_DIR}/src${CONTOUR_WASM_CXX_LIST_SEPARATOR}${PROJECT_SOURCE_DIR}/cmake")
        add_dependencies(vtextension_test cxx-daemon-probe_wasm)
        target_compile_definitions(vtextension_test PRIVATE VTEXTENSION_CXX_PROBE_MODULE="${_cxx_probe_wasm}")
        # The agent orchestrator's module (extensions/orchestrator, added before src/), run against the host's
        # services with the orchestrator's recorded fixtures. The measured store lives in the build tree, on the
        # disk the build is on: /tmp is often tmpfs.
        add_dependencies(vtextension_test orchestrator_wasm)
        target_compile_definitions(vtextension_test PRIVATE
            VTEXTENSION_ORCHESTRATOR_MODULE="${CONTOUR_ORCHESTRATOR_WASM_MODULE}"
            VTEXTENSION_ORCHESTRATOR_TESTDATA="${PROJECT_SOURCE_DIR}/extensions/orchestrator/core/testdata"
            VTEXTENSION_MEASURE_STORE_DIR="${CMAKE_BINARY_DIR}/measure-store")
    endif()
    # The tests run real guests, so they link the selected backend; its own tests are added by its directory.
    target_link_libraries(vtextension_test PRIVATE vtextension_runtime glaze::glaze core::testing Catch2::Catch2 contour_test_trace)

    # Compiles the headers other modules include (the GUI, vthost) without glaze on the include path: a public
    # header that pulled in ControlCodec.hpp, or anything else naming glaze, fails to build here, as it would in
    # the GUI. Built with vtextension_test, so that its suite cannot pass with such a leak.
    add_library(vtextension_public_headers_test OBJECT testing/PublicHeaders.cpp)
    target_link_libraries(vtextension_public_headers_test PRIVATE vtextension)
    add_dependencies(vtextension_test vtextension_public_headers_test)
    add_test(NAME vtextension_test COMMAND vtextension_test)

    if(NOT WIN32)
        # Catch2's TEST_CASE expands __COUNTER__, which Clang flags as a C2y extension under -Werror.
        target_compile_options(vtextension_test PRIVATE -Wno-c2y-extensions)
    endif()
endif()

# The backend this build selected (CONTOUR_WASM_RUNTIME, validated in the top-level CMakeLists.txt), and the one
# name everything that composes a program links: the GUI, contour_cli (`contour daemon`), bench-headless and the
# test executables. vtextension and vthost do not. Its target defines vtextension::wasm::makeDefaultBackend().
add_subdirectory(wasm/${CONTOUR_WASM_RUNTIME})
add_library(vtextension_runtime INTERFACE)
target_link_libraries(vtextension_runtime INTERFACE vtextension_${CONTOUR_WASM_RUNTIME})
