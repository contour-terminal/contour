# vtextension_host: the extension host's side of the protocol. HostServer serves one engine's runtimes, modules
# and instances over a control channel and its lanes; InProcessHostConnector runs one in this process, for the
# tests and for measuring the protocol without a process boundary; HostProcess is the extension host
# executable's body, HostBootstrap and Sandbox what that process does to itself before it serves. It links no
# engine: whoever composes a host hands it one (IHostedEngine), and its tests live in vtextension_remote_test
# (../remote/CMakeLists.txt), which links Wasmtime's.

set(_bootstrap_source HostBootstrap_posix.cpp)
if(WIN32)
    set(_bootstrap_source HostBootstrap_win32.cpp)
endif()

# Each system's sandbox (Sandbox.hpp) is a file of its own, and a value of PlatformSandbox; a system that has none
# yet runs the host unsandboxed, and its Hello says so. Linux's: where VTEXTENSION_LINUX_SANDBOX (../../CMakeLists.txt).
set(_sandbox_source Sandbox_none.cpp)
set(_platform_sandbox 0)
if(VTEXTENSION_LINUX_SANDBOX)
    set(_sandbox_source
        Sandbox_linux.cpp
        linux/Landlock.cpp
        linux/Landlock.hpp
        linux/SeccompFilter.cpp
        linux/SeccompFilter.hpp
        linux/SystemCallRows.cpp
        linux/SystemCallRows.hpp
    )
    set(_platform_sandbox 1)
endif()

add_library(vtextension_host STATIC
    ExecutorThread.cpp
    ExecutorThread.hpp
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
