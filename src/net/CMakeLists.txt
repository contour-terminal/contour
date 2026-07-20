set(net_SOURCES
    AsyncBufferedReader.cpp
    AsyncBufferedReader.h
    EventLoop.cpp
    EventLoop.h
    EventSource.h
    IListener.h
    ISocket.h
    IoResult.h
    WriteQueue.cpp
    WriteQueue.h
    PollEventSource.cpp
    PollEventSource.h
    Sockets.h
    WithTimeout.h
    platform/Clock.h
    platform/NativeHandle.h
    platform/SystemPipe.cpp
    platform/SystemPipe.h
    platform/WinsockInit.cpp
    platform/WinsockInit.h
    testing/InMemoryTransport.cpp
    testing/InMemoryTransport.h
    testing/ScriptedEventSource.h
)
if(WIN32)
    list(APPEND net_SOURCES
        windows/SocketsWin32.cpp
        windows/WindowsListener.cpp
        windows/WindowsListener.h
        windows/WindowsSocket.cpp
        windows/WindowsSocket.h
    )
else()
    list(APPEND net_SOURCES
        posix/PosixListener.cpp
        posix/PosixListener.h
        posix/PosixSocket.cpp
        posix/PosixSocket.h
        posix/SocketsPosix.cpp
        posix/UnixListener.cpp
        posix/UnixListener.h
    )
endif()

add_library(net STATIC ${net_SOURCES})
target_link_libraries(net PUBLIC coro crispy::core)
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
        Socket_test.cpp
        WriteQueue_test.cpp
    )
    if(NOT WIN32)
        list(APPEND net_TEST_SOURCES UnixSocket_test.cpp)
    endif()
    add_executable(net_test ${net_TEST_SOURCES})
    target_link_libraries(net_test net crispy::core Catch2::Catch2)
    add_test(NAME net_test COMMAND $<TARGET_FILE:net_test>)

    if(NOT WIN32)
        # Catch2's TEST_CASE expands __COUNTER__, which Clang flags as a C2y extension under -Werror.
        target_compile_options(net_test PRIVATE -Wno-c2y-extensions)
    endif()
endif()
