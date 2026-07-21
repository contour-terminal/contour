add_library(muxserver STATIC
    Daemon.cpp
    Daemon.h
    MuxServer.cpp
    MuxServer.h
    NativeSession.cpp
    NativeSession.h
    SessionHost.cpp
    SessionHost.h
    SocketPath.h
    TappingPty.h
    client/AttachClient.cpp
    client/AttachClient.h
    client/TtyRenderer.cpp
    client/TtyRenderer.h
    proto/Pdu.cpp
    proto/Pdu.h
    proto/Wire.cpp
    proto/Wire.h
    tmux/ControlModeParser.cpp
    tmux/ControlModeParser.h
    tmux/ControlOutput.cpp
    tmux/ControlOutput.h
    tmux/ControlSession.cpp
    tmux/ControlSession.h
    tmux/LayoutString.cpp
    tmux/LayoutString.h
    tmux/TmuxGateway.cpp
    tmux/TmuxGateway.h
)
target_link_libraries(muxserver PUBLIC
    net
    vtmux
    vtbackend
    vtpty
    crispy::core
)
target_include_directories(muxserver PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/include>
)

option(MUXSERVER_TESTING "Enables building of unittests for muxserver [default: ON]" ${CONTOUR_TESTING})
if(MUXSERVER_TESTING)
    enable_testing()
    add_executable(muxserver_test
        test_main.cpp
        SessionHost_test.cpp
        MuxServer_test.cpp
        NativeSession_test.cpp
        SocketPath_test.cpp
        client/AttachClient_test.cpp
        client/TtyRenderer_test.cpp
        proto/Pdu_test.cpp
        proto/Wire_test.cpp
        tmux/ControlModeParser_test.cpp
        tmux/ControlOutput_test.cpp
        tmux/ControlSession_test.cpp
        tmux/LayoutString_test.cpp
        tmux/TmuxGateway_test.cpp
    )
    target_link_libraries(muxserver_test muxserver crispy::core Catch2::Catch2)
    add_test(NAME muxserver_test COMMAND $<TARGET_FILE:muxserver_test>)

    if(NOT WIN32)
        # Catch2's TEST_CASE expands __COUNTER__, which Clang flags as a C2y extension under -Werror.
        target_compile_options(muxserver_test PRIVATE -Wno-c2y-extensions)
    endif()
endif()
