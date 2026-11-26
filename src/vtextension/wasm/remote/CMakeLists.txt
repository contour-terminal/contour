# vtextension_remote_wire: the protocol Contour speaks with its extension host process, and the channels it
# travels on. The codec (proto/) is pure bytes-in/PDUs-out and the transport (transport/) is the operating
# system's byte channels; neither links a WebAssembly engine or vtextension itself, so that both the parent and
# the helper process link them, and both are always built. On Windows the channel's owner-only pipe DACL
# needs advapi32.

# What both processes need of the operating system, one source per platform: the channel, the shared memory
# objects (the interrupt page and guest memory) and the wake signal.
set(_transport_sources
    transport/Channel_posix.cpp
    transport/ControlPage_posix.cpp
    transport/GuestMemoryMapping_posix.cpp
    transport/HandleGrant_posix.cpp
    transport/SharedObject_posix.cpp
    transport/WakeSignal_posix.cpp
)
if(WIN32)
    set(_transport_sources
        transport/Channel_win32.cpp
        transport/ControlPage_win32.cpp
        transport/GuestMemoryMapping_win32.cpp
        transport/HandleGrant_win32.cpp
        transport/SharedObject_win32.cpp
        transport/WakeSignal_win32.cpp
    )
endif()

# The codec's sources, compiled into the library and, with the fuzzer's flags, into the fuzz target below: one
# list, so that the fuzzer never decodes with a codec the library does not ship.
set(_codec_sources
    proto/Codec.cpp
    proto/Codec.hpp
    proto/LockdownSteps.cpp
    proto/LockdownSteps.hpp
    proto/Pdu.hpp
    proto/PduTable.hpp
    proto/Wire.hpp
)

add_library(vtextension_remote_wire STATIC
    ${_codec_sources}
    ../BuildInstrumentation.hpp
    transport/Channel.hpp
    transport/ControlPage.hpp
    transport/GuestMemoryMapping.cpp
    transport/GuestMemoryMapping.hpp
    transport/HandleGrant.hpp
    transport/HelperArguments.cpp
    transport/HelperArguments.hpp
    transport/OwnedHandle.hpp
    transport/PduStream.cpp
    transport/PduStream.hpp
    transport/SharedObject.cpp
    transport/SharedObject.hpp
    transport/SharedObjectName.hpp
    transport/WakeSignal.hpp
    transport/WouldBlock.hpp
    WarningLog.cpp
    WarningLog.hpp
    ${_transport_sources}
)
# The warning log (WarningLog.hpp), which a channel writes to as well as the parent's code.
target_link_libraries(vtextension_remote_wire PUBLIC core::log)
# The codec names the runtime vocabulary (WasmRuntime.hpp, ExtensionError.hpp), which is header-only and
# std-only: the include path is all it takes.
target_include_directories(vtextension_remote_wire PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/include>
)

if(WIN32)
    target_link_libraries(vtextension_remote_wire PRIVATE advapi32)
endif()

# How this build is instrumented: 0 not at all, 1 for coverage, 2 by a sanitizer (../BuildInstrumentation.hpp,
# its only reader). Public, so that both processes and everything between them agree on what the extension host
# caps, where its stderr goes and how its engine traps.
target_compile_definitions(vtextension_remote_wire PUBLIC
    CONTOUR_BUILD_INSTRUMENTATION=$<IF:$<BOOL:${CONTOUR_SANITIZE}>,2,$<IF:$<BOOL:${CONTOUR_COVERAGE}>,1,0>>)

# vtextension_remote_process: starting extension host processes and knowing when they end, one source per
# operating system. It stays std-only -- the wire library and core-cpp, no engine and no vtbackend -- so that it,
# and its tests, cross-build for Windows and run under wine. Systems with none of the four have no launcher yet.
set(_process_sources_Linux process/Process_linux.cpp process/Process_linux.hpp)
set(_process_sources_FreeBSD process/Process_freebsd.cpp)
set(_process_sources_Darwin process/Process_macos.cpp)
set(_process_sources_Windows
    process/AppContainer_win32.cpp process/AppContainer_win32.hpp process/LaunchRefusal_win32.cpp
    process/LaunchRefusal_win32.hpp process/LaunchSpelling_win32.cpp process/LaunchSpelling_win32.hpp
    process/Process_win32.cpp process/Process_win32.hpp)
set(_process_source "${_process_sources_${CMAKE_SYSTEM_NAME}}")
if(_process_source AND NOT WIN32)
    # What the POSIX launchers share.
    list(APPEND _process_source process/PosixLaunch.cpp process/PosixLaunch.hpp)
endif()
# What every system has of it: the lanes' watch, which the remote backend keeps and the supervisor reads, and the
# log of why no host serves, which the backend and the process connector both write to (through core::log).
add_library(vtextension_remote_process STATIC
    HostRefusals.cpp
    HostRefusals.hpp
    LaneWatch.cpp
    LaneWatch.hpp
)
target_link_libraries(vtextension_remote_process PUBLIC vtextension_remote_wire core::base core::log)
if(_process_source)
    # The supervisor and the process connector sit on the launcher: they name the connector's vocabulary
    # (HostConnector.hpp), which is header-only and std-only.
    target_sources(vtextension_remote_process PRIVATE
        process/ExtensionHostSupervisor.cpp
        process/ExtensionHostSupervisor.hpp
        process/HelperLocation.cpp
        process/HelperLocation.hpp
        process/HostTimers.cpp
        process/HostTimers.hpp
        process/ProcessHostConnector.cpp
        process/ProcessHostConnector.hpp
        process/ProcessLauncher.hpp
        ${_process_source}
    )
    target_link_libraries(vtextension_remote_process PUBLIC core::platform)
    # Windows: no library beyond the default ones. The AppContainer functions beyond advapi32's -- userenv's, and
    # combase's CoTaskMemFree() -- are looked up at run time (process/AppContainer_win32.cpp), so that no
    # executable linking the launcher imports a library that loads user32 as it starts.
    # Where the extension host is installed, from the directory contour is (../../../CMakeLists.txt).
    target_compile_definitions(vtextension_remote_process PUBLIC
        CONTOUR_EXTENSION_HOST_RELATIVE_DIR="${CONTOUR_EXTENSION_HOST_RELATIVE_DIR}")

    if(VTEXTENSION_TESTING)
        # The probe the launcher's tests start: it binds itself to its parent and caps itself as the host does,
        # reports what it was started with -- working directory, environment, caps, open handles -- on its
        # control channel, and exits once that channel ends. Never installed.
        # With --parent it is a hosting process instead: it supervises a helper of its own, for the tests of a
        # helper outliving its parent.
        add_executable(extension_host_probe process/testing/LaunchProbe.cpp process/testing/RecordingLauncher.hpp
            process/testing/SandboxProbe.hpp)
        target_link_libraries(extension_host_probe PRIVATE
            vtextension_host vtextension_remote_process vtextension_remote_wire)
        if(WIN32)
            # --contained tries a loopback connection.
            target_link_libraries(extension_host_probe PRIVATE ws2_32)
            # The loader probe (process/testing/LoaderProbe.cpp): says which libraries a process may load. Its
            # C runtime linked in, it imports kernel32 alone, so that it starts wherever a process can.
            add_executable(extension_host_loader_probe process/testing/LoaderProbe.cpp)
            set_target_properties(extension_host_loader_probe PROPERTIES
                MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        # Where a sandbox is built (VTEXTENSION_{LINUX,FREEBSD,MACOS}_SANDBOX), the probe also locks
        # itself down as the host does, and process_test proves the sandbox with it and tests its pieces.
        if(VTEXTENSION_LINUX_SANDBOX OR VTEXTENSION_FREEBSD_SANDBOX OR VTEXTENSION_MACOS_SANDBOX)
            target_sources(extension_host_probe PRIVATE process/testing/SandboxProbe.cpp)
            target_link_libraries(extension_host_probe PRIVATE vtextension_host_sanitizer_options)
        endif()

        # The launcher against the probe, the transport's pieces the helper inherits, and the test launchers;
        # no engine, so that it cross-builds and runs under wine.
        add_executable(vtextension_process_test
            ${PROJECT_SOURCE_DIR}/src/vtextension/test_main.cpp
            HostRefusals_test.cpp
            LaneWatch_test.cpp
            process/AppContainer_test.cpp
            process/ExtensionHostSupervisor_test.cpp
            process/HelperLocation_test.cpp
            process/HostComposition_test.cpp
            process/HostTimers_test.cpp
            process/ProcessHostConnector_test.cpp
            process/ProcessLauncher_test.cpp
            process/testing/FakeProcessLauncher.cpp
            process/testing/FakeProcessLauncher.hpp
            process/testing/LaunchedHelper.cpp
            process/testing/LaunchedHelper.hpp
            process/testing/RecordingLauncher.hpp
            transport/ControlPage_test.cpp
            transport/GuestMemoryMapping_test.cpp
            transport/HandleGrant_test.cpp
            transport/HelperArguments_test.cpp
            transport/SharedObject_test.cpp
            transport/WakeSignal_test.cpp
        )
        # vtextension_host on every system: the launcher's tests read the host's cap policy (HostBootstrap.hpp),
        # whose header needs the sandbox definition only that target carries. vtextension_out_of_process for the
        # composition's tests, which create backends but load nothing through a helper.
        target_link_libraries(vtextension_process_test PRIVATE
            vtextension_host vtextension_out_of_process vtextension_remote_process vtextension_remote_wire
            core::testing Catch2::Catch2 contour_test_trace)
        if(VTEXTENSION_LINUX_SANDBOX)
            target_sources(vtextension_process_test PRIVATE
                ../host/linux/Landlock_test.cpp
                ../host/linux/SeccompFilter_test.cpp
                ../host/linux/testing/BpfEvaluator.hpp
            )
        endif()
        if(VTEXTENSION_LINUX_SANDBOX OR VTEXTENSION_FREEBSD_SANDBOX OR VTEXTENSION_MACOS_SANDBOX)
            target_sources(vtextension_process_test PRIVATE process/SandboxProbe_test.cpp)
        endif()
        if(WIN32)
            target_sources(vtextension_process_test PRIVATE
                process/LaunchSpelling_test.cpp
                process/LoaderProbe_test.cpp
                process/testing/PeImports_win32.cpp
                process/testing/PeImports_win32.hpp
            )
            add_dependencies(vtextension_process_test extension_host_loader_probe)
            target_compile_definitions(vtextension_process_test PRIVATE
                VTEXTENSION_LOADER_PROBE_PATH="$<TARGET_FILE:extension_host_loader_probe>")
            # Sandbox.windowsHelperRunsContained listens on loopback for the contained probe.
            target_link_libraries(vtextension_process_test PRIVATE ws2_32)
        endif()
        add_dependencies(vtextension_process_test extension_host_probe)
        target_compile_definitions(vtextension_process_test PRIVATE
            VTEXTENSION_PROBE_PATH="$<TARGET_FILE:extension_host_probe>")
        add_test(NAME vtextension_process_test COMMAND vtextension_process_test)
        if(NOT WIN32)
            # Catch2's TEST_CASE expands __COUNTER__, which Clang flags as a C2y extension under -Werror.
            target_compile_options(vtextension_process_test PRIVATE -Wno-c2y-extensions)
        endif()
    endif()
endif()

# vtextension_remote: the backend that runs extensions in a separate host, over the channels above. It links no
# engine and starts no process itself: a connector (HostConnector.hpp) the composition root injects does.
add_library(vtextension_remote STATIC
    CopyPathMemory.cpp
    CopyPathMemory.hpp
    HelperIncarnation.cpp
    HelperIncarnation.hpp
    HostConnector.hpp
    RemoteBackend.cpp
    RemoteBackend.hpp
    RemoteKindState.cpp
    RemoteKindState.hpp
    SharedGuestMemory.cpp
    SharedGuestMemory.hpp
)
target_link_libraries(vtextension_remote PUBLIC vtextension vtextension_remote_wire vtextension_remote_process)

if(_process_source)
    # vtextension_out_of_process: how a program runs its extensions out of process -- one supervisor, one
    # kind and one connector for every backend (process/HostComposition.hpp) -- and the default backend
    # composed of them (DefaultBackend.cpp), which a program links through vtextension_runtime
    # (../../CMakeLists.txt). Links no engine: the engine runs in the helper.
    add_library(vtextension_out_of_process STATIC
        DefaultBackend.cpp
        process/HostComposition.cpp
        process/HostComposition.hpp
    )
    target_link_libraries(vtextension_out_of_process PUBLIC vtextension_remote vtextension_remote_process vtpty)
elseif(CONTOUR_EXTENSIONS AND CONTOUR_EXTENSION_HOST STREQUAL "out-of-process")
    message(FATAL_ERROR "CONTOUR_EXTENSION_HOST is out-of-process, but Contour cannot start processes on "
                        "${CMAKE_SYSTEM_NAME} yet; choose in-process.")
endif()

option(VTEXTENSION_FUZZING "Builds the extension host protocol's libFuzzer target (Clang only) [default: OFF]" OFF)
if(VTEXTENSION_FUZZING)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "VTEXTENSION_FUZZING needs Clang, for -fsanitize=fuzzer.")
    endif()
    # Decodes its input as every (direction, channel) pair, and checks that what decodes encodes again to
    # the same PDU. CI runs the fixed-seed mutation case in vtextension_test instead.
    #
    # The codec is compiled into the fuzzer itself, with the fuzzer's coverage instrumentation, rather than
    # linked from vtextension_remote_wire: the fuzzer is guided by the decoder's branches, and the library
    # every other target links stays uninstrumented (-fsanitize=fuzzer-no-link there would need the
    # coverage runtime in each of them).
    #
    # Not an OBJECT library both share: the fuzzer's copy needs the fuzzer's compile flags, which the
    # library's must not have. Both compile _codec_sources, so a new codec source reaches both.
    add_executable(vtextension_remote_fuzz proto/Codec_fuzz.cpp ${_codec_sources})
    target_include_directories(vtextension_remote_fuzz PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_compile_options(vtextension_remote_fuzz PRIVATE -fsanitize=fuzzer)
    target_link_options(vtextension_remote_fuzz PRIVATE -fsanitize=fuzzer)
endif()

if(VTEXTENSION_TESTING AND TARGET vtextension_wasmtime_hosted_engine)
    # The host's server, and the backend that reaches it, driven over real channels on a real engine:
    # Wasmtime's, the engine a host process runs, reached through its directory's neutral test probe
    # (vtextension_wasmtime_hosted_engine).
    #
    # Beside them, the suites vtextension_test runs on the default backend, here composed by
    # testing/RemoteDefaultBackend.cpp as the remote backend over hosts in this process: their real guests
    # run through the protocol.
    add_executable(vtextension_remote_test
        ${PROJECT_SOURCE_DIR}/src/vtextension/test_main.cpp
        ../host/HostProcess_test.cpp
        ../host/HostServer_test.cpp
        RegistryRemote_test.cpp
        RemoteBackend_test.cpp
        SharedGuestMemory_test.cpp
        testing/RemoteDefaultBackend.cpp
        ${PROJECT_SOURCE_DIR}/src/vtextension/Bundled_test.cpp
        ${PROJECT_SOURCE_DIR}/src/vtextension/ControlImports_test.cpp
        ${PROJECT_SOURCE_DIR}/src/vtextension/Engine_test.cpp
        ${PROJECT_SOURCE_DIR}/src/vtextension/ExtensionHost_test.cpp
        ${PROJECT_SOURCE_DIR}/src/vtextension/ExtensionWorkerPool_test.cpp
        ${PROJECT_SOURCE_DIR}/src/vtextension/Instance_test.cpp
    )
    target_link_libraries(vtextension_remote_test PRIVATE
        vtextension vtextension_remote vtextension_remote_wire vtextension_host vtextension_wasmtime vtextension_wasmtime_hosted_engine
        contour::wasmtime glaze::glaze core::testing Catch2::Catch2 contour_test_trace)
    if(CONTOUR_WASM_GUESTS)
        # The guests vtextension_test reads, from where it reads them (../../CMakeLists.txt).
        foreach(_guest IN ITEMS echo hostile sleeper spawner)
            add_dependencies(vtextension_remote_test ${_guest}_wasm)
        endforeach()
        add_dependencies(vtextension_remote_test contour_bundled_extensions)
        target_compile_definitions(vtextension_remote_test PRIVATE
            VTEXTENSION_TEST_MODULE_DIR="${VTEXTENSION_TEST_MODULE_DIR}")
    endif()
    if(_process_source)
        # The process connector, driven through the handshake by a host a fake launcher plays.
        target_sources(vtextension_remote_test PRIVATE
            process/LockdownRefusal_test.cpp
            process/testing/FakeProcessLauncher.cpp
            process/testing/FakeProcessLauncher.hpp
        )
    endif()
    add_test(NAME vtextension_remote_test COMMAND vtextension_remote_test)
    if(NOT WIN32)
        # Catch2's TEST_CASE expands __COUNTER__, which Clang flags as a C2y extension under -Werror.
        target_compile_options(vtextension_remote_test PRIVATE -Wno-c2y-extensions)
    endif()
endif()

if(VTEXTENSION_TESTING AND TARGET vtextension_wamr_engine)
    # The contract through the protocol on WAMR's engine: the remote backend over hosts serving in this process.
    # A binary of its own, apart from vtextension_remote_test, as one executable never links both engines: each
    # defines the standard wasm.h C API, which MSVC's linker refuses twice (LNK2005) and an ELF linker silently
    # resolves to whichever engine it meets first. scripts/check-wasm-runtime-containment.py keeps it that way.
    #
    # Beside it, the host's tests that need no engine at all (its executor threads and its sandbox): built
    # wherever vtextension_host is, as WAMR's engine always is, and not only where Wasmtime is found.
    add_executable(vtextension_remote_wamr_test
        ${PROJECT_SOURCE_DIR}/src/vtextension/test_main.cpp
        ../host/ExecutorThread_test.cpp
        ../host/Sandbox_test.cpp
        ../wamr/RemoteWamrEngine_test.cpp
    )
    target_link_libraries(vtextension_remote_wamr_test PRIVATE
        vtextension vtextension_remote vtextension_remote_wire vtextension_host vtextension_wamr_engine
        core::testing Catch2::Catch2 contour_test_trace)
    add_test(NAME vtextension_remote_wamr_test COMMAND vtextension_remote_wamr_test)
    if(NOT WIN32)
        # Catch2's TEST_CASE expands __COUNTER__, which Clang flags as a C2y extension under -Werror.
        target_compile_options(vtextension_remote_wamr_test PRIVATE -Wno-c2y-extensions)
    endif()
endif()

if(VTEXTENSION_TESTING AND CONTOUR_WASM_RUNTIME STREQUAL "wasmtime" AND _process_source)
    # The extension host executable itself (../../../contour-extension-host, on Wasmtime's engine, as here),
    # started as Contour starts it. Links no engine: the engine runs in the helper. vtextension_host only for
    # the build's cap policy (HostBootstrap.hpp), which tells what the helper caps.
    #
    # Beside them, the contract and the suites vtextension_test runs on the default backend, here composed by
    # testing/ProcessDefaultBackend.cpp as the remote backend over real helper processes: their real guests
    # run through a process boundary.
    add_executable(vtextension_helper_test
        ${PROJECT_SOURCE_DIR}/src/vtextension/test_main.cpp
        Helper_test.cpp
        HostileMemory_test.cpp
        ProcessBackend_test.cpp
        Hostility_test.cpp
        ProcessMeasure_test.cpp
        ReviewFocusProcess_test.cpp
        Teardown_test.cpp
        process/testing/HelperComposition.cpp
        process/testing/HelperComposition.hpp
        process/testing/LaunchedHelper.cpp
        process/testing/LaunchedHelper.hpp
        testing/ProcessDefaultBackend.cpp
        ${PROJECT_SOURCE_DIR}/src/vtextension/Bundled_test.cpp
        ${PROJECT_SOURCE_DIR}/src/vtextension/ControlImports_test.cpp
        ${PROJECT_SOURCE_DIR}/src/vtextension/Engine_test.cpp
        ${PROJECT_SOURCE_DIR}/src/vtextension/ExtensionHost_test.cpp
        ${PROJECT_SOURCE_DIR}/src/vtextension/ExtensionWorkerPool_test.cpp
        ${PROJECT_SOURCE_DIR}/src/vtextension/Instance_test.cpp
    )
    target_link_libraries(vtextension_helper_test PRIVATE
        vtextension vtextension_host vtextension_remote vtextension_remote_process vtextension_remote_wire
        glaze::glaze core::testing Catch2::Catch2 contour_test_trace)
    add_dependencies(vtextension_helper_test contour-extension-host contour-extension-host-hostile extension_host_probe)
    target_compile_definitions(vtextension_helper_test PRIVATE
        VTEXTENSION_HELPER_PATH="$<TARGET_FILE:contour-extension-host>"
        VTEXTENSION_HOSTILE_HELPER_PATH="$<TARGET_FILE:contour-extension-host-hostile>"
        VTEXTENSION_PROBE_PATH="$<TARGET_FILE:extension_host_probe>")
    if(CONTOUR_WASM_GUESTS)
        # The guests vtextension_test reads, from where it reads them (../../CMakeLists.txt).
        foreach(_guest IN ITEMS echo hostile sleeper spawner)
            add_dependencies(vtextension_helper_test ${_guest}_wasm)
        endforeach()
        add_dependencies(vtextension_helper_test contour_bundled_extensions)
        target_compile_definitions(vtextension_helper_test PRIVATE
            VTEXTENSION_TEST_MODULE_DIR="${VTEXTENSION_TEST_MODULE_DIR}")
    endif()
    if(WIN32)
        # The startup imports of the extension host and of the probe, and what the extension host loads as it
        # starts, loaded contained (process/StartupImports_test.cpp, process/LoaderProbe_test.cpp).
        target_sources(vtextension_helper_test PRIVATE
            process/LoaderProbe_test.cpp
            process/StartupImports_test.cpp
            process/testing/PeImports_win32.cpp
            process/testing/PeImports_win32.hpp
        )
        add_dependencies(vtextension_helper_test extension_host_loader_probe)
        target_compile_definitions(vtextension_helper_test PRIVATE
            VTEXTENSION_LOADER_PROBE_PATH="$<TARGET_FILE:extension_host_loader_probe>")
    endif()
    add_test(NAME vtextension_helper_test COMMAND vtextension_helper_test)
    if(NOT WIN32)
        # Catch2's TEST_CASE expands __COUNTER__, which Clang flags as a C2y extension under -Werror.
        target_compile_options(vtextension_helper_test PRIVATE -Wno-c2y-extensions)
    endif()
endif()

if(VTEXTENSION_TESTING AND TARGET vtextension_wamr_helper_engine AND _process_source)
    # The extension host on WAMR's engine, started as Contour starts it: contour-extension-host where the build
    # runs WAMR's, its test-only twin contour-extension-host-wamr where it runs another
    # (../../../contour-extension-host). The contract, the bundled extensions and the process cases of
    # vtextension_helper_test, composed by testing/ProcessDefaultBackend.cpp over that helper, locked down where
    # the build has a sandbox; and the measurements, so that this helper is measured against WAMR in process.
    set(_wamr_helper contour-extension-host-wamr)
    if(CONTOUR_WASM_RUNTIME STREQUAL "wamr")
        set(_wamr_helper contour-extension-host)
    endif()
    add_executable(vtextension_wamr_helper_test
        ${PROJECT_SOURCE_DIR}/src/vtextension/test_main.cpp
        ProcessBackend_test.cpp
        ProcessMeasure_test.cpp
        ReviewFocusProcess_test.cpp
        WamrHelper_test.cpp
        process/testing/HelperComposition.cpp
        process/testing/HelperComposition.hpp
        process/testing/LaunchedHelper.cpp
        process/testing/LaunchedHelper.hpp
        testing/ProcessDefaultBackend.cpp
        ${PROJECT_SOURCE_DIR}/src/vtextension/Bundled_test.cpp
    )
    target_link_libraries(vtextension_wamr_helper_test PRIVATE
        vtextension vtextension_host vtextension_remote vtextension_remote_process vtextension_remote_wire
        glaze::glaze core::testing Catch2::Catch2 contour_test_trace)
    add_dependencies(vtextension_wamr_helper_test ${_wamr_helper} contour-extension-host-hostile)
    target_compile_definitions(vtextension_wamr_helper_test PRIVATE
        VTEXTENSION_HELPER_PATH="$<TARGET_FILE:${_wamr_helper}>"
        VTEXTENSION_HOSTILE_HELPER_PATH="$<TARGET_FILE:contour-extension-host-hostile>")
    if(CONTOUR_WASM_GUESTS)
        # The guests vtextension_test reads, from where it reads them (../../CMakeLists.txt).
        foreach(_guest IN ITEMS echo hostile sleeper spawner)
            add_dependencies(vtextension_wamr_helper_test ${_guest}_wasm)
        endforeach()
        add_dependencies(vtextension_wamr_helper_test contour_bundled_extensions)
        target_compile_definitions(vtextension_wamr_helper_test PRIVATE
            VTEXTENSION_TEST_MODULE_DIR="${VTEXTENSION_TEST_MODULE_DIR}")
    endif()
    if(WIN32)
        # The startup imports of this extension host and of the probe, and what this extension host loads as it
        # starts, loaded contained (process/StartupImports_test.cpp, process/LoaderProbe_test.cpp).
        target_sources(vtextension_wamr_helper_test PRIVATE
            process/LoaderProbe_test.cpp
            process/StartupImports_test.cpp
            process/testing/PeImports_win32.cpp
            process/testing/PeImports_win32.hpp
        )
        add_dependencies(vtextension_wamr_helper_test extension_host_probe extension_host_loader_probe)
        target_compile_definitions(vtextension_wamr_helper_test PRIVATE
            VTEXTENSION_PROBE_PATH="$<TARGET_FILE:extension_host_probe>"
            VTEXTENSION_LOADER_PROBE_PATH="$<TARGET_FILE:extension_host_loader_probe>")
    endif()
    add_test(NAME vtextension_wamr_helper_test COMMAND vtextension_wamr_helper_test)
    if(NOT WIN32)
        # Catch2's TEST_CASE expands __COUNTER__, which Clang flags as a C2y extension under -Werror.
        target_compile_options(vtextension_wamr_helper_test PRIVATE -Wno-c2y-extensions)
    endif()
endif()
