# vtextension: hosts WebAssembly status-line extensions on WAMR. Qt-free, so that both the GUI
# (src/contour) and the daemon (src/vthost) can link it.

set(vtextension_SOURCES
    BundledExtensions.cpp
    CanonicalAbi.cpp
    CommandLine.cpp
    CommandPolicy.cpp
    CompiledExtension.cpp
    Engine.cpp
    EventQueue.cpp
    ExtensionComposition.cpp
    ExtensionConsent.cpp
    ExtensionGrantStore.cpp
    ExtensionHost.cpp
    ExtensionWorkerPool.cpp
    HostState.cpp
    Instance.cpp
    Manifest.cpp
    Runner.cpp
    SessionExtensions.cpp
    StatusLineAdapter.cpp
    Surface.cpp
    wasm/wamr/WamrBackend.cpp
    wasm/wamr/WamrInstance.cpp
    wasm/wamr/WamrModule.cpp
    wasm/wamr/WamrThreadEnv.cpp
)

set(vtextension_HEADERS
    AtomicSharedPtr.hpp
    BundledExtensions.hpp
    CanonicalAbi.hpp
    CommandLine.hpp
    CommandPolicy.hpp
    CompiledExtension.hpp
    DeadlineGuard.hpp
    Engine.hpp
    Event.hpp
    EventQueue.hpp
    ExtensionComposition.hpp
    ExtensionConsent.hpp
    ExtensionError.hpp
    ExtensionGrantStore.hpp
    ExtensionHost.hpp
    ExtensionLimits.hpp
    ExtensionScheduler.hpp
    ExtensionWorkerPool.hpp
    HostBindings.hpp
    HostState.hpp
    IHostServices.hpp
    Instance.hpp
    Manifest.hpp
    Placement.hpp
    Runner.hpp
    SessionExtensions.hpp
    StatusLineAdapter.hpp
    StatuslineWorld.hpp
    Surface.hpp
    Utf8.hpp
    wasm/WasmRuntime.hpp
    wasm/wamr/WamrBackend.hpp
    wasm/wamr/WamrHandles.hpp
    wasm/wamr/WamrRuntime.hpp
    wasm/wamr/WamrThreadEnv.hpp
)

add_library(vtextension STATIC ${vtextension_SOURCES} ${vtextension_HEADERS})
target_compile_definitions(vtextension PUBLIC CONTOUR_EXTENSIONS=1)
target_include_directories(vtextension PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/include>
)
# vtpty for PageSize, the render call's surface size, and the CommandCapture extensions run commands through;
# vtbackend for the Surface's private terminal; core::async for the thread a session's modules are loaded on.
target_link_libraries(vtextension PUBLIC vtbackend vtpty core::async core::base core::log core::platform PRIVATE contour::wamr)

option(VTEXTENSION_TESTING "Enables building of unittests for vtextension [default: ON]" ${CONTOUR_TESTING})
if(VTEXTENSION_TESTING)
    enable_testing()

    add_executable(vtextension_test
        test_main.cpp
        Bundled_test.cpp
        BundledExtensions_test.cpp
        CanonicalAbi_test.cpp
        CommandLine_test.cpp
        CommandPolicy_test.cpp
        DeadlineGuard_test.cpp
        Engine_test.cpp
        EventQueue_test.cpp
        ExtensionComposition_test.cpp
        ExtensionConsent_test.cpp
        ExtensionGrantStore_test.cpp
        ExtensionHost_test.cpp
        ExtensionWorkerPool_test.cpp
        HostState_test.cpp
        Instance_test.cpp
        Isolation_test.cpp
        Manifest_test.cpp
        Runner_test.cpp
        SessionExtensions_test.cpp
        StatusLineAdapter_test.cpp
        Surface_test.cpp
        Wit_test.cpp
        wasm/wamr/WamrBackend_test.cpp
        testing/ManualScheduler.hpp
        testing/QueuedOwner.hpp
        testing/RunnerHarness.hpp
        testing/StandardStreamsCapture.hpp
        testing/TestModules.hpp
        wasm/testing/BackendContract.hpp
    )
    # The WIT package is the normative spec of the extension ABI; the tests read it from the source tree.
    target_compile_definitions(vtextension_test PRIVATE VTEXTENSION_WIT_DIR="${PROJECT_SOURCE_DIR}/wit/statusline")
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
    # The engine's own tests drive WAMR's C API directly, so the test links it too.
    target_link_libraries(vtextension_test vtextension contour::wamr core::testing Catch2::Catch2)
    add_test(NAME vtextension_test COMMAND $<TARGET_FILE:vtextension_test>)

    if(NOT WIN32)
        # Catch2's TEST_CASE expands __COUNTER__, which Clang flags as a C2y extension under -Werror.
        target_compile_options(vtextension_test PRIVATE -Wno-c2y-extensions)
    endif()
endif()
