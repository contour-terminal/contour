# vtextension_host: the extension host's side of the protocol. HostServer serves one engine's runtimes, modules
# and instances over a control channel and its lanes; InProcessHostConnector runs one in this process, for the
# tests and for measuring the protocol without a process boundary. It links no engine: whoever composes a host
# hands it one (IHostedEngine), and its tests live in vtextension_remote_test (../remote/CMakeLists.txt), which
# links Wasmtime's.

add_library(vtextension_host STATIC
    HostServer.cpp
    HostServer.hpp
    IHostedEngine.hpp
    InProcessHost.cpp
    InProcessHost.hpp
    ../remote/HostConnector.hpp
)
target_link_libraries(vtextension_host PUBLIC vtextension_remote_wire PRIVATE core::base)
