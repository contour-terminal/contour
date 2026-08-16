set(net_SOURCES
    AsyncBufferedReader.cpp
    AsyncBufferedReader.hpp
    DefaultEventSource.cpp
    DefaultEventSource.hpp
    Diagnostics.cpp
    Diagnostics.hpp
    EpollEventSource.cpp
    EpollEventSource.hpp
    EventLoop.cpp
    EventLoop.hpp
    EventSource.hpp
    HttpServer.cpp
    HttpServer.hpp
    IListener.hpp
    ISocket.hpp
    IoResult.hpp
    KqueueEventSource.cpp
    KqueueEventSource.hpp
    detail/ScopeGuard.hpp
    detail/WouldBlock.hpp
    WriteQueue.cpp
    WriteQueue.hpp
    PollEventSource.cpp
    PollEventSource.hpp
    Sockets.hpp
    SplitSocket.hpp
    Tls.cpp
    Tls.hpp
    WithTimeout.hpp
    platform/Clock.hpp
    platform/NativeHandle.hpp
    platform/PeerAddress.hpp
    platform/SystemPipe.cpp
    platform/SystemPipe.hpp
    platform/WinsockInit.cpp
    platform/WinsockInit.hpp
    testing/InMemoryTransport.cpp
    testing/InMemoryTransport.hpp
    testing/EventSourceBackends.hpp
    testing/ScriptedEventSource.hpp
)
if(WIN32)
    list(APPEND net_SOURCES
        platform/WindowsLoopback.cpp
        platform/WindowsLoopback.hpp
        windows/SocketsWin32.cpp
        windows/WindowsListener.cpp
        windows/WindowsListener.hpp
        windows/WindowsSocket.cpp
        windows/WindowsSocket.hpp
    )
else()
    list(APPEND net_SOURCES
        posix/AcceptLoop.cpp
        posix/AcceptLoop.hpp
        posix/PosixListener.cpp
        posix/PosixListener.hpp
        posix/PosixSocket.cpp
        posix/PosixSocket.hpp
        posix/SocketsPosix.cpp
        posix/UnixListener.cpp
        posix/UnixListener.hpp
    )
endif()

add_library(net STATIC ${net_SOURCES})
# OpenSSL backs the TLS ISocket decorator (net/Tls.*). It stays behind the
# ITlsContext DI seam, so it is PRIVATE: no OpenSSL type crosses net's headers.
find_package(OpenSSL REQUIRED)
target_link_libraries(net PUBLIC coro)
target_link_libraries(net PRIVATE OpenSSL::SSL OpenSSL::Crypto)
if(WIN32)
    target_link_libraries(net PUBLIC ws2_32)
endif()
target_include_directories(net PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/include>
)

option(NET_TESTING "Enables building of unittests for net [default: ON]" ${CONTOUR_TESTING})
if(NET_TESTING)
    enable_testing()
    set(net_TEST_SOURCES
        test_main.cpp
        AsyncBufferedReader_test.cpp
        EventLoop_test.cpp
        EventSourceParity_test.cpp
        HttpServer_test.cpp
        Socket_test.cpp
        Tls_test.cpp
        WriteQueue_test.cpp
    )
    if(NOT WIN32)
        list(APPEND net_TEST_SOURCES FdPassing_test.cpp UnixSocket_test.cpp)
    endif()
    add_executable(net_test ${net_TEST_SOURCES})
    target_link_libraries(net_test net Catch2::Catch2)
    add_test(NAME net_test COMMAND $<TARGET_FILE:net_test>)
    set_tests_properties(net_test PROPERTIES LABELS daemon)

    if(NOT WIN32)
        # Catch2's TEST_CASE expands __COUNTER__, which Clang flags as a C2y extension under -Werror.
        target_compile_options(net_test PRIVATE -Wno-c2y-extensions)
    endif()
endif()
