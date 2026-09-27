add_library(vthost STATIC
    ClientSizePolicy.hpp
    ImageWire.hpp
    MouseWire.hpp
    Daemon.cpp
    Daemon.hpp
    GracefulClose.hpp
    LoopDrain.hpp
    ConnectionAcceptor.cpp
    ConnectionAcceptor.hpp
    NativeSession.cpp
    NativeSession.hpp
    PduPump.hpp
    SessionHost.cpp
    SessionHost.hpp
    ServiceControl.cpp
    ServiceControl.hpp
    ServiceControl_win32.cpp
    SessionSettings.cpp
    SessionSettings.hpp
    SocketPath.hpp
    TappingPty.hpp
    client/NativeClient.cpp
    client/NativeClient.hpp
    client/LayoutReconstruction.cpp
    client/LayoutReconstruction.hpp
    imsg/CommandArgv.cpp
    imsg/CommandArgv.hpp
    imsg/Identify.cpp
    imsg/Identify.hpp
    imsg/ImsgCodec.cpp
    imsg/ImsgCodec.hpp
    client/ScreenMirror.cpp
    client/ScreenMirror.hpp
    GridWire.cpp
    GridWire.hpp
    proto/Pdu.cpp
    proto/Pdu.hpp
    proto/PduTrace.cpp
    proto/PduTrace.hpp
    proto/Wire.cpp
    proto/Wire.hpp
    tmux/ControlModeParser.cpp
    tmux/ControlModeParser.hpp
    tmux/ControlModeSpawn.cpp
    tmux/ControlModeSpawn.hpp
    tmux/ControlOutput.cpp
    tmux/ControlOutput.hpp
    tmux/ControlSession.cpp
    tmux/ControlSession.hpp
    tmux/ImsgServer.cpp
    tmux/ImsgServer.hpp
    tmux/LayoutString.cpp
    tmux/LayoutString.hpp
    tmux/PaneSink.hpp
    tmux/TmuxClientModel.cpp
    tmux/TmuxClientModel.hpp
    tmux/TmuxGateway.cpp
    tmux/TmuxGateway.hpp
)
target_link_libraries(vthost PUBLIC
    core::async
    core::base
    core::log
    core::net
    vtworkspace
    vtbackend
    vtpty
    crispy::core
)
# The daemon serves, and the client dials, TLS; no header of vthost's names it.
target_link_libraries(vthost PRIVATE core::net_tls)
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
        GracefulClose_test.cpp
        LoopDrain_test.cpp
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
        testing/GridParity.hpp
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
    target_link_libraries(vthost_test vthost core::net_tls core::testing Catch2::Catch2)
    add_test(NAME vthost_test COMMAND $<TARGET_FILE:vthost_test>)
    # `ctest -L daemon` selects everything daemon mode must pass to be trusted -- see the same
    # label on vtworkspace, a layer it is built on. The other two, core::async and core::net, are
    # vendored in vendor/core-cpp, and their suites run in core-cpp's own CI.
    set_tests_properties(vthost_test PROPERTIES LABELS daemon)

    if(NOT WIN32)
        # Catch2's TEST_CASE expands __COUNTER__, which Clang flags as a C2y extension under -Werror.
        target_compile_options(vthost_test PRIVATE -Wno-c2y-extensions)
    endif()
endif()
