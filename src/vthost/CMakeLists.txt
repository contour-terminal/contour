add_library(vthost STATIC
    AccessFile.cpp
    AccessFile.hpp
    AttachPlan.cpp
    AttachPlan.hpp
    ByteStream.cpp
    ByteStream.hpp
    ClientCensus.hpp
    ClientSizePolicy.hpp
    ConfigReload.hpp
    FlushPolicy.hpp
    IdleExitWatcher.hpp
    CommandBlockWire.hpp
    DaemonControl.cpp
    DaemonControl.hpp
    DaemonControlWire.hpp
    DaemonBinary.cpp
    DaemonBinary.hpp
    DaemonImage.cpp
    DaemonImage.hpp
    DaemonImageLeases_posix.cpp
    DaemonImageLeases_win32.cpp
    DaemonLifecycle.hpp
    DaemonRuntime.cpp
    DaemonStarter.cpp
    DaemonStarter.hpp
    DaemonRuntime.hpp
    ExtensionHosting.hpp
    FilePrivacy.cpp
    FilePrivacy.hpp
    FilePrivacy_win32.cpp
    PermissionWire.hpp
    PlatformStreams.hpp
    PlatformStreams_posix.cpp
    PlatformStreams_win32.cpp
    ProfileCatalog.hpp
    ImageWire.hpp
    TimeWire.hpp
    MouseWire.hpp
    InputEchoWire.hpp
    NotifyWire.hpp
    WireEnum.hpp
    Daemon.cpp
    Daemon.hpp
    GracefulClose.hpp
    HistorySeed.cpp
    HistorySeed.hpp
    HostIdentity.cpp
    HostIdentity.hpp
    LoopDrain.hpp
    ProcessImage_native.cpp
    RerunPolicy.hpp
    ConnectionAcceptor.cpp
    ConnectionAcceptor.hpp
    NativeSession.cpp
    NativeSession.hpp
    ObjectRegistry.cpp
    ObjectRegistry.hpp
    PduPump.hpp
    SessionHost.cpp
    SessionHost.hpp
    CommandRunner.hpp
    CommandRunner_posix.cpp
    CommandRunner_win32.cpp
    ServiceControl.cpp
    ServiceControl.hpp
    ServiceControl_launchd.cpp
    ServiceControl_launchd.hpp
    ServiceControl_systemd.cpp
    ServiceControl_systemd.hpp
    ServiceControl_win32.cpp
    RestoredCounts.hpp
    SessionSettings.cpp
    SessionEnd.hpp
    SessionSettings.hpp
    SessionSnapshot.hpp
    SessionSnapshotCodec.cpp
    SessionSnapshotCodec.hpp
    SessionSnapshotStore.cpp
    SessionSnapshotStore.hpp
    SnapshotSchedule.hpp
    WorkspaceCapture.cpp
    WorkspaceCapture.hpp
    WorkspacePersistence.cpp
    WorkspacePersistence.hpp
    WorkspacePersistenceConfig.hpp
    ShutdownFunnel.cpp
    ShutdownFunnel.hpp
    ShutdownSignals.cpp
    ShutdownSignals.hpp
    SocketPath.hpp
    SshTransport.cpp
    SshTransport.hpp
    StatePath.hpp
    StderrTail.hpp
    StdioAttach.cpp
    StdioAttach.hpp
    StdioChild.hpp
    StdioChild_posix.cpp
    StdioChild_win32.cpp
    StdioPreamble.cpp
    StdioPreamble.hpp
    TabEdits.cpp
    TabEdits.hpp
    TappingPty.hpp
    Transport.cpp
    Transport.hpp
    client/ClientSupervisor.cpp
    client/ClientSupervisor.hpp
    client/ConnectionState.hpp
    client/ControlClient.cpp
    client/ControlClient.hpp
    client/DaemonControlClient.cpp
    client/DaemonControlClient.hpp
    control/BlockWaiters.cpp
    control/BlockWaiters.hpp
    control/ControlService.cpp
    control/ControlService.hpp
    control/EventHub.cpp
    control/EventHub.hpp
    control/LoopControlBridge.cpp
    control/LoopControlBridge.hpp
    control/PaneText.cpp
    control/PaneText.hpp
    control/ProgramStatusConvert.hpp
    client/DaemonRestart.cpp
    client/DaemonRestart.hpp
    client/ReleaseDrift.cpp
    client/ReleaseDrift.hpp
    client/Liveness.hpp
    client/NativeClient.cpp
    client/NativeClient.hpp
    client/LayoutReconstruction.cpp
    client/LayoutReconstruction.hpp
    latency/EchoProtocol.hpp
    latency/Gate.cpp
    latency/Gate.hpp
    latency/KeystrokeProbe.cpp
    latency/KeystrokeProbe.hpp
    latency/Report.cpp
    latency/Report.hpp
    latency/StartupTimeline.cpp
    latency/StartupTimeline.hpp
    latency/Stats.cpp
    latency/Stats.hpp
    imsg/CommandArgv.cpp
    imsg/CommandArgv.hpp
    imsg/Identify.cpp
    imsg/Identify.hpp
    imsg/ImsgCodec.cpp
    imsg/ImsgCodec.hpp
    client/ScreenMirror.cpp
    client/ScreenMirror.hpp
    client/TabPresentation.hpp
    client/ViewportOrigin.hpp
    GridWire.cpp
    GridWire.hpp
    proto/Negotiation.cpp
    proto/Negotiation.hpp
    proto/Pdu.cpp
    proto/Pdu.hpp
    proto/PduTrace.cpp
    proto/PduTrace.hpp
    proto/ReleasedVersions.hpp
    proto/Unsupported.cpp
    proto/Unsupported.hpp
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
    vtcontrol
    core::async
    core::base
    core::log
    core::net
    core::platform
    vtworkspace
    vtbackend
    vtpty
    crispy::core
)
# The daemon serves, and the client dials, TLS; no header of vthost's names it.
target_link_libraries(vthost PRIVATE core::net_tls)
# The control API's JSON (vtcontrol/Json.hpp), read and written in .cpp files only: no header of vthost's
# names glaze either.
target_link_libraries(vthost PRIVATE glaze::glaze)
if(WIN32)
    # The Task Scheduler 2.0 backend of ServiceControl is COM (taskschd), hence ole32/oleaut32
    # for the apartment and the BSTR/VARIANT allocators.
    target_link_libraries(vthost PRIVATE taskschd ole32 oleaut32 advapi32)
endif()
target_include_directories(vthost PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/include>
)
# The status-line extensions of the hosted sessions. Without them nothing of vtextension is linked, and
# vthost's own types keep one layout in both builds (@see ExtensionHosting.hpp).
if(CONTOUR_EXTENSIONS)
    target_sources(vthost PRIVATE
        DaemonExtensions.cpp
        DaemonExtensions.hpp
        client/DaemonGrantReplies.cpp
        client/DaemonGrantReplies.hpp
    )
    target_link_libraries(vthost PUBLIC vtextension)
endif()

option(VTHOST_TESTING "Enables building of unittests for vthost [default: ON]" ${CONTOUR_TESTING})
if(VTHOST_TESTING)
    enable_testing()
    add_executable(vthost_test
        test_main.cpp
        AccessFile_test.cpp
        AttachPlan_test.cpp
        ByteStream_test.cpp
        TabEdits_test.cpp
        GracefulClose_test.cpp
        HostIdentity_test.cpp
        LoopDrain_test.cpp
        ObjectRegistry_test.cpp
        SessionHost_test.cpp
        FlushPolicy_test.cpp
        SessionSettings_test.cpp
        SessionEnd_test.cpp
        SessionSnapshotCodec_test.cpp
        SessionSnapshotStore_test.cpp
        FilePrivacy_test.cpp
        WorkspaceCapture_test.cpp
        HistorySeed_test.cpp
        RerunPolicy_test.cpp
        WorkspaceRestore_test.cpp
        SnapshotSchedule_test.cpp
        WorkspacePersistence_test.cpp
        ConnectionAcceptor_test.cpp
        CommandBlockWire_test.cpp
        CommandRunner_test.cpp
        DaemonControl_test.cpp
        DaemonRuntime_test.cpp
        DaemonControlWire_test.cpp
        DaemonBinary_test.cpp
        DaemonImage_test.cpp
        DaemonStarter_test.cpp
        Daemon_test.cpp
        ImageWire_test.cpp
        NotifyWire_test.cpp
        TimeWire_test.cpp
        WireEnum_test.cpp
        IdleExitWatcher_test.cpp
        LastSessionWatcher_test.cpp
        NativeSession_test.cpp
        PduPump_test.cpp
        ServiceControl_launchd_test.cpp
        ServiceControl_systemd_test.cpp
        ServiceControl_test.cpp
        SocketPath_test.cpp
        SshTransport_test.cpp
        StatePath_test.cpp
        StdioAttach_test.cpp
        StdioChild_test.cpp
        StdioPreamble_test.cpp
        Token_test.cpp
        Transport_test.cpp
        testing/GridParity.cpp
        testing/GridParity.hpp
        testing/ShippedCodec.hpp
        testing/PduExamples.hpp
        testing/PreNegotiationCodec.hpp
        testing/RecordedSnapshots.hpp
        testing/RecordingFilePrivacy.hpp
        testing/RecordingDurableFiles.hpp
        testing/FaultyFileSystem.hpp
        testing/RecordedTranscripts.hpp
        testing/ControlHarness.hpp
        testing/FakeCommandRunner.hpp
        testing/FakeDaemonControl.hpp
        testing/FakeExtensionHosting.hpp
        testing/FakeImageLeases.hpp
        testing/FakeProfileCatalog.hpp
        testing/ForwardingFileSystem.hpp
        testing/GatedFileSystem.hpp
        testing/MemoryByteStream.hpp
        testing/StagingFileSystem.hpp
        testing/TestHelpers.hpp
        client/ClientSupervisor_test.cpp
        client/ControlClient_test.cpp
        client/DaemonControlClient_test.cpp
        control/ControlService_test.cpp
        control/LoopControlBridge_test.cpp
        control/EventHub_test.cpp
        control/PaneText_test.cpp
        control/ProgramStatusConvert_test.cpp
        client/DaemonRestart_test.cpp
        client/ReleaseDrift_test.cpp
        client/Liveness_test.cpp
        client/NativeClient_test.cpp
        client/LayoutReconstruction_test.cpp
        client/ScreenMirror_test.cpp
        client/ViewportOrigin_test.cpp
        imsg/ImsgCodec_test.cpp
        latency/EchoProtocol_test.cpp
        latency/Gate_test.cpp
        latency/KeystrokeProbe_test.cpp
        latency/Report_test.cpp
        latency/StartupTimeline_test.cpp
        latency/Stats_test.cpp
        proto/Compatibility_test.cpp
        proto/Negotiation_test.cpp
        proto/Pdu_test.cpp
        proto/PduTrace_test.cpp
        proto/Unsupported_test.cpp
        proto/Wire_test.cpp
        tmux/ControlModeParser_test.cpp
        tmux/ControlModeSpawn_test.cpp
        tmux/ControlOutput_test.cpp
        tmux/ControlSession_test.cpp
        tmux/ImsgServer_test.cpp
        tmux/LayoutString_test.cpp
        tmux/TmuxClientModel_test.cpp
        tmux/TmuxGateway_test.cpp
    )
    target_link_libraries(vthost_test vthost core::net_tls glaze::glaze core::testing Catch2::Catch2)
    # vthost links no WebAssembly runtime; its tests compose one, as `contour daemon` does.
    if(CONTOUR_EXTENSIONS)
        target_link_libraries(vthost_test vtextension_runtime)
    endif()
    # The release check reads which releases metainfo.xml names (@see proto/ReleasedVersions.hpp).
    target_compile_definitions(vthost_test PRIVATE CONTOUR_METAINFO_PATH="${PROJECT_SOURCE_DIR}/metainfo.xml")
    # The daemon's extensions are tested end to end with a guest that runs a command, where guests are built.
    if(CONTOUR_EXTENSIONS AND TARGET spawner_wasm)
        target_compile_definitions(vthost_test PRIVATE
            VTHOST_TEST_SPAWNER_WASM="${CONTOUR_CARGO_TARGET_DIR}/${CONTOUR_WASM_TARGET}/release/spawner.wasm")
        add_dependencies(vthost_test spawner_wasm)
    endif()
    # The daemon's font behaviour is checked with the echo guest, which renders the state it reads.
    if(CONTOUR_EXTENSIONS AND TARGET echo_wasm)
        target_compile_definitions(vthost_test PRIVATE
            VTHOST_TEST_ECHO_WASM="${CONTOUR_CARGO_TARGET_DIR}/${CONTOUR_WASM_TARGET}/release/echo.wasm")
        add_dependencies(vthost_test echo_wasm)
    endif()
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

# The latency benchmark: the echo child, the headless and the end-to-end legs, and the `bench-latency`
# executable that runs them. Never installed, so it never reaches a package.
option(VTHOST_BUILD_BENCH_LATENCY "Builds bench-latency, which compares the daemon path with in-process [default: OFF]" OFF)
if(VTHOST_BUILD_BENCH_LATENCY)
    add_library(vthost_bench_latency STATIC
        latency/BenchSupport.cpp
        latency/BenchSupport.hpp
        latency/E2eBench.cpp
        latency/E2eBench.hpp
        latency/EchoChild.cpp
        latency/EchoChild.hpp
        latency/HeadlessBench.cpp
        latency/HeadlessBench.hpp
    )
    target_link_libraries(vthost_bench_latency PUBLIC vthost)
    add_executable(bench-latency latency/bench-latency.cpp)
    target_compile_definitions(bench-latency PRIVATE CONTOUR_VERSION_STRING="${CONTOUR_VERSION_STRING}")
    target_link_libraries(bench-latency vthost_bench_latency core::cli)
    # `bench-latency e2e` and the display-gated runs find their executables beside the other tools.
    if(VTHOST_TESTING)
        add_executable(bench_latency_test test_main.cpp latency/BenchSupport_test.cpp)
        target_link_libraries(bench_latency_test vthost_bench_latency core::testing Catch2::Catch2)
        if(NOT WIN32)
            target_compile_options(bench_latency_test PRIVATE -Wno-c2y-extensions)
        endif()
        add_test(NAME bench_latency_test COMMAND $<TARGET_FILE:bench_latency_test>)
        set_tests_properties(bench_latency_test PROPERTIES LABELS "daemon;bench")
        # A short headless run of each leg: a crash in the daemon leg or the in-process leg fails the suite.
        add_test(NAME bench_latency_headless_smoke COMMAND bench-latency headless runs 1 keys 20)
        set_tests_properties(bench_latency_headless_smoke PROPERTIES LABELS "daemon;bench" TIMEOUT 120
            PASS_REGULAR_EXPRESSION "keystroke +daemon +[0-9]+ +[0-9.]+")
        # A name that is not a workload must fail, naming the ones that are.
        add_test(NAME bench_latency_rejects_unknown_workload
                 COMMAND bench-latency headless workload no-such-workload)
        set_tests_properties(bench_latency_rejects_unknown_workload PROPERTIES LABELS "daemon;bench"
            WILL_FAIL TRUE)
    endif()
    message(STATUS "[vthost] Build bench-latency: ON")
endif()
