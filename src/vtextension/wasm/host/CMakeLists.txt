# vtextension_host: the extension host's side of the protocol. HostServer serves one engine's runtimes, modules
# and instances over a control channel and its lanes; InProcessHostConnector runs one in this process, for the
# tests and for measuring the protocol without a process boundary; HostProcess is the extension host
# executable's body, HostBootstrap and Sandbox what that process does to itself before it serves. It links no
# engine: whoever composes a host hands it one (IHostedEngine). Its tests live in vtextension_remote_test
# (../remote/CMakeLists.txt), which links Wasmtime's, and those that need no engine in vtextension_remote_wamr_test,
# which is built wherever this library is.

set(_bootstrap_source HostBootstrap_posix.cpp)
if(WIN32)
    set(_bootstrap_source HostBootstrap_win32.cpp)
endif()

# Each system's sandbox (Sandbox.hpp) is a file of its own, and a value of PlatformSandbox; a system that has none
# yet runs the host unsandboxed, and its Hello says so. Linux's: where VTEXTENSION_LINUX_SANDBOX; FreeBSD's: where
# VTEXTENSION_FREEBSD_SANDBOX; macOS's: where VTEXTENSION_MACOS_SANDBOX (all ../../CMakeLists.txt); Windows': always,
# as it verifies the AppContainer its parent started it in and says `Unsandboxed` when there is none.
# The values of PlatformSandbox (Sandbox.hpp), in its enumerators' order: CONTOUR_PLATFORM_SANDBOX is the index of
# this build's in this list. A list rather than numbers in each branch below, so that the order is written once
# on this side; Sandbox.hpp names the same order on its own.
set(_platform_sandboxes None Linux FreeBSD MacOS Windows)
set(_sandbox_source Sandbox_none.cpp)
set(_platform_sandbox_name None)
if(WIN32)
    set(_sandbox_source Sandbox_win32.cpp)
    set(_platform_sandbox_name Windows)
elseif(VTEXTENSION_FREEBSD_SANDBOX)
    set(_sandbox_source Sandbox_freebsd.cpp)
    set(_platform_sandbox_name FreeBSD)
elseif(VTEXTENSION_MACOS_SANDBOX)
    set(_sandbox_source Sandbox_macos.cpp)
    set(_platform_sandbox_name MacOS)
elseif(VTEXTENSION_LINUX_SANDBOX)
    set(_sandbox_source
        Sandbox_linux.cpp
        linux/Landlock.cpp
        linux/Landlock.hpp
        linux/SeccompFilter.cpp
        linux/SeccompFilter.hpp
        linux/SystemCallRows.cpp
        linux/SystemCallRows.hpp
    )
    set(_platform_sandbox_name Linux)
endif()
list(FIND _platform_sandboxes ${_platform_sandbox_name} _platform_sandbox)

# Out of process without a sandbox, every extension host would be refused for running unconfined, and no extension
# would run at all: say so now rather than at the first extension.
if(CONTOUR_EXTENSION_HOST STREQUAL "out-of-process" AND _platform_sandbox_name STREQUAL "None")
    message(FATAL_ERROR "CONTOUR_EXTENSION_HOST is out-of-process, but the extension host has no sandbox on "
                        "${CMAKE_SYSTEM_NAME} (${CMAKE_SYSTEM_PROCESSOR}), so Contour would refuse every extension "
                        "host it starts. Choose CONTOUR_EXTENSION_HOST=in-process.")
endif()

add_library(vtextension_host STATIC
    ContainerSandbox.cpp
    ContainerSandbox.hpp
    DescriptorRights.cpp
    DescriptorRights.hpp
    ExecutorThread.cpp
    ExecutorThread.hpp
    GuestCode.hpp
    HelperEngine.hpp
    HostBootstrap.cpp
    HostBootstrap.hpp
    ${_bootstrap_source}
    HostProcess.cpp
    HostProcess.hpp
    HostServer.cpp
    HostServer.hpp
    IHostedEngine.hpp
    InProcessHost.cpp
    InProcessHost.hpp
    InterruptListener.cpp
    InterruptListener.hpp
    Sandbox.cpp
    Sandbox.hpp
    ${_sandbox_source}
    macos/SandboxProfile.hpp
    ../remote/HostConnector.hpp
)
target_link_libraries(vtextension_host PUBLIC vtextension_remote_wire vtextension_memory_provision PRIVATE core::base)
target_compile_definitions(vtextension_host PUBLIC CONTOUR_PLATFORM_SANDBOX=${_platform_sandbox})
if(CONTOUR_COVERAGE)
    # Where a coverage build writes its counters: the one directory its sandboxed host may still write to.
    target_compile_definitions(vtextension_host PRIVATE CONTOUR_COVERAGE_OBJECT_ROOT="${CMAKE_BINARY_DIR}")
endif()

# The memory a host hands the engine an instance runs on (MemoryProvision.hpp): a target of its own, so that an
# engine's backend, which Contour also links to run extensions in its own process, needs it and not the host.
add_library(vtextension_memory_provision STATIC
    MemoryProvision.cpp
    MemoryProvision.hpp
)
target_link_libraries(vtextension_memory_provision PUBLIC vtextension_remote_wire)

# What a sanitizer build's sandboxed process defaults its sanitizer to (SanitizerOptions.cpp): objects, linked
# into each executable that locks itself down -- the extension host and the probe that locks down as it does.
# Only the Linux filter needs them, and only ELF names them so; elsewhere the target is empty.
if(VTEXTENSION_LINUX_SANDBOX)
    add_library(vtextension_host_sanitizer_options OBJECT SanitizerOptions.cpp)
    target_link_libraries(vtextension_host_sanitizer_options PRIVATE vtextension_remote_wire)
else()
    add_library(vtextension_host_sanitizer_options INTERFACE)
endif()
