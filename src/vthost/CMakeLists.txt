add_library(vthost STATIC
    ClientSizePolicy.h
    ImageWire.h
    MouseWire.h
    Daemon.cpp
    Daemon.h
    ConnectionAcceptor.cpp
    ConnectionAcceptor.h
    NativeSession.cpp
    NativeSession.h
    PduPump.h
    SessionHost.cpp
    SessionHost.h
    ServiceControl.cpp
    ServiceControl.h
    ServiceControl_win32.cpp
    SessionSettings.cpp
    SessionSettings.h
    SocketPath.h
    TappingPty.h
    client/NativeClient.cpp
    client/NativeClient.h
    client/LayoutReconstruction.cpp
    client/LayoutReconstruction.h
    imsg/CommandArgv.cpp
    imsg/CommandArgv.h
    imsg/Identify.cpp
    imsg/Identify.h
    imsg/ImsgCodec.cpp
    imsg/ImsgCodec.h
    client/ScreenMirror.cpp
    client/ScreenMirror.h
    GridWire.cpp
    GridWire.h
    proto/Pdu.cpp
    proto/Pdu.h
    proto/PduTrace.cpp
    proto/PduTrace.h
    proto/Wire.cpp
    proto/Wire.h
    tmux/ControlModeParser.cpp
    tmux/ControlModeParser.h
    tmux/ControlModeSpawn.cpp
    tmux/ControlModeSpawn.h
    tmux/ControlOutput.cpp
    tmux/ControlOutput.h
    tmux/ControlSession.cpp
    tmux/ControlSession.h
    tmux/ImsgServer.cpp
    tmux/ImsgServer.h
    tmux/LayoutString.cpp
    tmux/LayoutString.h
    tmux/PaneSink.h
    tmux/TmuxClientModel.cpp
    tmux/TmuxClientModel.h
    tmux/TmuxGateway.cpp
    tmux/TmuxGateway.h
)
target_link_libraries(vthost PUBLIC
    net
    vtworkspace
    vtbackend
    vtpty
    crispy::core
)
if(WIN32)
    # The Task Scheduler 2.0 backend of ServiceControl is COM (taskschd), hence ole32/oleaut32
    # for the apartment and the BSTR/VARIANT allocators.
    target_link_libraries(vthost PRIVATE taskschd ole32 oleaut32)
endif()
target_include_directories(vthost PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/include>
)

option(VTHOST_TESTING "Enables building of unittests for vthost [default: ON]" ${CONTOUR_TESTING})
if(VTHOST_TESTING)
    enable_testing()
    add_executable(vthost_test
        test_main.cpp
        SessionHost_test.cpp
        SessionSettings_test.cpp
        ConnectionAcceptor_test.cpp
        Daemon_test.cpp
        ImageWire_test.cpp
        LastSessionWatcher_test.cpp
        NativeSession_test.cpp
        PduPump_test.cpp
        ServiceControl_test.cpp
        SocketPath_test.cpp
        Token_test.cpp
        testing/GridParity.cpp
        testing/GridParity.h
        client/NativeClient_test.cpp
        client/LayoutReconstruction_test.cpp
        client/ScreenMirror_test.cpp
        imsg/ImsgCodec_test.cpp
        proto/Pdu_test.cpp
        proto/PduTrace_test.cpp
        proto/Wire_test.cpp
        tmux/ControlModeParser_test.cpp
        tmux/ControlOutput_test.cpp
        tmux/ControlSession_test.cpp
        tmux/ImsgServer_test.cpp
        tmux/LayoutString_test.cpp
        tmux/TmuxClientModel_test.cpp
        tmux/TmuxGateway_test.cpp
    )
    target_link_libraries(vthost_test vthost crispy::core Catch2::Catch2)
    add_test(NAME vthost_test COMMAND $<TARGET_FILE:vthost_test>)
    # `ctest -L daemon` selects everything daemon mode must pass to be trusted -- see the same
    # label on net, coro and vtworkspace, the layers it is built on.
    set_tests_properties(vthost_test PROPERTIES LABELS daemon)

    if(NOT WIN32)
        # Catch2's TEST_CASE expands __COUNTER__, which Clang flags as a C2y extension under -Werror.
        target_compile_options(vthost_test PRIVATE -Wno-c2y-extensions)
    endif()
endif()
