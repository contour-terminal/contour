# vtextension: hosts WebAssembly status-line extensions on a runtime-neutral interface (wasm/WasmRuntime.hpp).
# Qt-free, so that both the GUI (src/contour) and the daemon (src/vthost) can link it. It links no WebAssembly
# runtime: each backend is its own target (wasm/<runtime>/, and wasm/remote/ for extension host processes),
# and a program links the one CONTOUR_EXTENSION_HOST selects through vtextension_runtime, defined at the bottom.

set(vtextension_SOURCES
    BundledExtensions.cpp
    CanonicalAbi.cpp
    CommandLine.cpp
    CommandPolicy.cpp
    CompiledExtension.cpp
    ControlImports.cpp
    Engine.cpp
    EventQueue.cpp
    ExtensionComposition.cpp
    ExtensionConsent.cpp
    ExtensionGrantStore.cpp
    ExtensionHost.cpp
    ExtensionRegistry.cpp
    ExtensionWorkerPool.cpp
    HostBreaker.cpp
    HostState.cpp
    Instance.cpp
    Manifest.cpp
    Runner.cpp
    SessionExtensions.cpp
    StatusLineAdapter.cpp
    Surface.cpp
    wasm/wasi/Preview1Shim.cpp
    wasm/wasi/SystemEnvironment.cpp
)

set(vtextension_HEADERS
    AtomicSharedPtr.hpp
    BundledExtensions.hpp
    CanonicalAbi.hpp
    CommandLine.hpp
    CommandPolicy.hpp
    CompiledExtension.hpp
    ControlCodec.hpp
    ControlImports.hpp
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
    ExtensionRegistry.hpp
    ExtensionScheduler.hpp
    ExtensionWorkerPool.hpp
    GuestMemoryReads.hpp
    HostBindings.hpp
    HostBreaker.hpp
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
    wasm/BackendConfig.hpp
    wasm/DefaultBackend.hpp
    wasm/EngineFeatures.hpp
    wasm/InstantiationHooks.hpp
    wasm/WasiAllowList.hpp
    wasm/WasmBinary.hpp
    wasm/WasmRuntime.hpp
    wasm/wasi/Preview1Shim.hpp
    wasm/wasi/Preview1Signatures.hpp
    wasm/wasi/SystemEnvironment.hpp
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
if(WIN32)
    # BCryptGenRandom: the random source of the WASI shim's system environment (wasm/wasi/SystemEnvironment.cpp).
    target_link_libraries(vtextension PRIVATE bcrypt)
endif()

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
        ControlCodec_test.cpp
        ControlImports_test.cpp
        DeadlineGuard_test.cpp
        Engine_test.cpp
        EventQueue_test.cpp
        ExtensionComposition_test.cpp
        ExtensionConsent_test.cpp
        ExtensionError_test.cpp
        ExtensionGrantStore_test.cpp
        ExtensionHost_test.cpp
        ExtensionRegistry_test.cpp
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
        wasm/WasmBinary_test.cpp
        wasm/remote/CopyPathMemory_test.cpp
        wasm/remote/proto/Codec_test.cpp
        wasm/remote/transport/Channel_test.cpp
        wasm/remote/transport/PduStream_test.cpp
        wasm/wasi/Preview1Shim_test.cpp
        testing/FakeGuestMemory.hpp
        testing/FakeWasmBackend.hpp
        testing/Harness.hpp
        testing/LaneReport.hpp
        testing/ManualScheduler.hpp
        testing/QueuedOwner.hpp
        testing/RunnerHarness.hpp
        testing/StandardStreamsCapture.hpp
        testing/TestModules.hpp
        wasm/testing/BackendContract.hpp
        wasm/testing/FeatureModules.hpp
        wasm/testing/PreparedGeneration.hpp
        wasm/testing/RenderMeasurement.hpp
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
    # The tests run real guests in this process, on WAMR's backend, named rather than the program's selection:
    # they start no extension host process. Its own tests are added by its directory.
    target_link_libraries(vtextension_test PRIVATE vtextension_wamr vtextension_remote vtextension_remote_wire glaze::glaze core::testing Catch2::Catch2 contour_test_trace)

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

# The engines' backends: WAMR's always, as the tests run guests in this process on it; Wasmtime's wherever one
# was found (../../cmake/ContourThirdParties.cmake).
add_subdirectory(wasm/wamr)
if(CONTOUR_HAVE_WASMTIME)
    add_subdirectory(wasm/wasmtime)
endif()

# Whether the extension host locks itself down with the Linux sandbox (wasm/host/Sandbox_linux.cpp): Linux, on the
# architectures whose system calls its filter numbers (host/linux/SystemCallRows.cpp). Keyed on those alone, as
# FreeBSD's and macOS's are on their systems, and not on _CONTOUR_SANDBOXED_PLATFORMS (../../CMakeLists.txt): taking
# a row out of that table changes where extensions run by default, and must never compile a sandbox out. Read by
# both directories below.
set(VTEXTENSION_LINUX_SANDBOX OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|aarch64|arm64)$")
    set(VTEXTENSION_LINUX_SANDBOX ON)
endif()
# Whether it locks itself down with FreeBSD's (wasm/host/Sandbox_freebsd.cpp): on FreeBSD, on every architecture,
# as Capsicum names no system call by number. Built, tested and run by the extension host there although FreeBSD is
# not yet a row of _CONTOUR_SANDBOXED_PLATFORMS: that row is what makes extensions run out of process by default,
# and it is added once FreeBSD's CI has run the sandbox's tests green.
set(VTEXTENSION_FREEBSD_SANDBOX OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    set(VTEXTENSION_FREEBSD_SANDBOX ON)
    # That sandbox caps the host's whole address space (RLIMIT_AS) at twice the memory its guests may hold and
    # some headroom (wasm/host/HostBootstrap.hpp, addressSpaceCap()), which fits WAMR. Wasmtime reserves
    # gigabytes of address space for each guest memory and its guard, which no such cap leaves it.
    if(CONTOUR_EXTENSIONS AND CONTOUR_WASM_RUNTIME STREQUAL "wasmtime")
        message(FATAL_ERROR "CONTOUR_WASM_RUNTIME is wasmtime, which the extension host's FreeBSD sandbox cannot "
                            "run: it caps the host's address space below what Wasmtime reserves for guest memory. "
                            "Configure with CONTOUR_WASM_RUNTIME=wamr.")
    endif()
endif()

# The protocol and channels between Contour and its extension host process, and the host's server; no engine,
# always built. After the backends: the host's tests run on Wasmtime's engine, where there is one.
add_subdirectory(wasm/remote)
add_subdirectory(wasm/host)

# The backend a program runs its extensions on, and the one name every program that composes one links: the
# GUI and `contour daemon` (../contour/CMakeLists.txt) and bench-headless. vtextension, vthost and the test
# executables do not; a test names the backend it runs on. Its target defines
# vtextension::wasm::makeDefaultBackend(): out of process, the remote backend over extension host processes
# (vtextension_out_of_process, wasm/remote/), which links no engine; in process, the engine
# CONTOUR_WASM_RUNTIME names.
add_library(vtextension_runtime INTERFACE)
if(CONTOUR_EXTENSION_HOST STREQUAL "out-of-process")
    target_link_libraries(vtextension_runtime INTERFACE vtextension_out_of_process)
else()
    target_link_libraries(vtextension_runtime INTERFACE vtextension_${CONTOUR_WASM_RUNTIME})
endif()
