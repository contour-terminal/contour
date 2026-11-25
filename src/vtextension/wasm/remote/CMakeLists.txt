# vtextension_remote_wire: the protocol Contour speaks with its extension host process, and the channels it
# travels on. The codec (proto/) is pure bytes-in/PDUs-out and the transport (transport/) is the operating
# system's byte channels; neither links a WebAssembly engine or vtextension itself, so that both the parent and
# the helper process link them, and both are always built. On Windows the channel's owner-only pipe DACL
# needs advapi32.

# What both processes need of the operating system, one source per platform: the channel, the interrupt page and
# the wake signal.
set(_transport_sources
    transport/Channel_posix.cpp
    transport/ControlPage_posix.cpp
    transport/WakeSignal_posix.cpp
)
if(WIN32)
    set(_transport_sources
        transport/Channel_win32.cpp
        transport/ControlPage_win32.cpp
        transport/WakeSignal_win32.cpp
    )
endif()

# The codec's sources, compiled into the library and, with the fuzzer's flags, into the fuzz target below: one
# list, so that the fuzzer never decodes with a codec the library does not ship.
set(_codec_sources
    proto/Codec.cpp
    proto/Codec.hpp
    proto/Pdu.hpp
    proto/PduTable.hpp
    proto/Wire.hpp
)

add_library(vtextension_remote_wire STATIC
    ${_codec_sources}
    transport/Channel.hpp
    transport/ControlPage.hpp
    transport/HelperArguments.cpp
    transport/HelperArguments.hpp
    transport/PduStream.cpp
    transport/PduStream.hpp
    transport/WakeSignal.hpp
    transport/WouldBlock.hpp
    ${_transport_sources}
)
# The codec names the runtime vocabulary (WasmRuntime.hpp, ExtensionError.hpp), which is header-only and
# std-only: the include path is all it takes.
target_include_directories(vtextension_remote_wire PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/include>
)

if(WIN32)
    target_link_libraries(vtextension_remote_wire PRIVATE advapi32)
endif()

if(VTEXTENSION_TESTING)
    # The transport's pieces the helper inherits; no engine, so that it cross-builds and runs under wine.
    add_executable(vtextension_process_test
        ${PROJECT_SOURCE_DIR}/src/vtextension/test_main.cpp
        transport/ControlPage_test.cpp
        transport/HelperArguments_test.cpp
        transport/WakeSignal_test.cpp
    )
    target_link_libraries(vtextension_process_test PRIVATE
        vtextension_remote_wire core::testing Catch2::Catch2 contour_test_trace)
    add_test(NAME vtextension_process_test COMMAND vtextension_process_test)
    if(NOT WIN32)
        # Catch2's TEST_CASE expands __COUNTER__, which Clang flags as a C2y extension under -Werror.
        target_compile_options(vtextension_process_test PRIVATE -Wno-c2y-extensions)
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
)
target_link_libraries(vtextension_remote PUBLIC vtextension vtextension_remote_wire)

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
        ../host/ExecutorThread_test.cpp
        ../host/HostServer_test.cpp
        RegistryRemote_test.cpp
        RemoteBackend_test.cpp
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
    add_test(NAME vtextension_remote_test COMMAND vtextension_remote_test)
    if(NOT WIN32)
        # Catch2's TEST_CASE expands __COUNTER__, which Clang flags as a C2y extension under -Werror.
        target_compile_options(vtextension_remote_test PRIVATE -Wno-c2y-extensions)
    endif()
endif()
