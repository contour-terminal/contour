set(net_SOURCES
    EventLoop.cpp
    EventLoop.h
    EventSource.h
    IoResult.h
    PollEventSource.cpp
    PollEventSource.h
    WithTimeout.h
    platform/Clock.h
    platform/NativeHandle.h
    platform/SystemPipe.cpp
    platform/SystemPipe.h
    platform/WinsockInit.cpp
    platform/WinsockInit.h
    testing/ScriptedEventSource.h
)

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
    add_executable(net_test
        test_main.cpp
        EventLoop_test.cpp
    )
    target_link_libraries(net_test net crispy::core Catch2::Catch2)
    add_test(NAME net_test COMMAND $<TARGET_FILE:net_test>)

    if(NOT WIN32)
        # Catch2's TEST_CASE expands __COUNTER__, which Clang flags as a C2y extension under -Werror.
        target_compile_options(net_test PRIVATE -Wno-c2y-extensions)
    endif()
endif()
