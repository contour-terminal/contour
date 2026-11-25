# vtextension_host: the extension host's side of the protocol. HostServer serves one engine's runtimes, modules
# and instances over a control channel and its lanes; InProcessHostConnector runs one in this process, for the
# tests and for measuring the protocol without a process boundary; HostProcess is the extension host
# executable's body, and HostBootstrap what that process does to itself before it serves. It links no engine:
# whoever composes a host hands it one (IHostedEngine), and its tests live in vtextension_remote_test
# (../remote/CMakeLists.txt), which links Wasmtime's.

set(_bootstrap_source HostBootstrap_posix.cpp)
if(WIN32)
    set(_bootstrap_source HostBootstrap_win32.cpp)
endif()

add_library(vtextension_host STATIC
    ExecutorThread.cpp
    ExecutorThread.hpp
    HelperEngine.hpp
    HostBootstrap.hpp
    ${_bootstrap_source}
    HostProcess.cpp
    HostProcess.hpp
    HostServer.cpp
    HostServer.hpp
    IHostedEngine.hpp
    InProcessHost.cpp
    InProcessHost.hpp
    ../remote/HostConnector.hpp
)
target_link_libraries(vtextension_host PUBLIC vtextension_remote_wire PRIVATE core::base)

# A sanitizer's shadow memory and its threads exceed any cap the host could apply to itself (HostBootstrap.hpp):
# public, so that every user of BuildCapPolicy sees the same value.
if(CONTOUR_SANITIZE)
    target_compile_definitions(vtextension_host PUBLIC CONTOUR_SANITIZED_BUILD)
endif()
