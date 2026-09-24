# SPDX-License-Identifier: Apache-2.0

# core::net: contour's event loop, sockets, TLS and HTTP server (contour 6777ff05), as contour has
# them but for the namespaces and core::platform in place of contour's net/platform/. Phase B
# replaces the EventSource API with IoBackend and merges fastcached's sockets into it. Three targets,
# each held to its row in cmake/CoreCppModules.cmake:
#
#   core::net_types  NetError and IoResult: header-only, and built everywhere, Emscripten included;
#   core::net        everything else. Its row says PLATFORMS wasm-subset: under single-threaded
#                    Emscripten only the files in the SOURCES_EMSCRIPTEN lists below build, which is
#                    why the subset's sources appear in both lists. The loop, the sockets and the
#                    readiness backends join it in Tasks B4 and B5;
#   core::net_tls    the TLS decorator, only with CORE_CPP_WITH_TLS. OpenSSL is PRIVATE to it, and no
#                    OpenSSL type appears in its header.
core_cpp_add_module(net_types KIND INTERFACE
    HEADERS
        IoResult.hpp
        NetError.hpp)

# Winsock backs the sockets on Windows. PRIVATE, because no Winsock type reaches a public header; a
# static library's private link still reaches the link line of the program that uses it.
set(_coreCppNetPrivateLibs "")
if(WIN32)
    list(APPEND _coreCppNetPrivateLibs ws2_32)
endif()

# Threads::Threads, and PUBLIC for the same reason core::base declares it: net's own headers
# (EventLoop.hpp above all) expose std::mutex and std::thread, so consumers instantiate them too.
# net states it itself rather than inheriting it through core::platform and core::base, because it
# is what net's own headers need. Free on glibc, fatal on FreeBSD, where the symbols live in libthr.
# (contour's src/net/CMakeLists.txt, 6777ff05) Not under single-threaded Emscripten, where linking it
# would force -pthread, and with it SharedArrayBuffer, onto every consumer: CORE_CPP_USE_THREADS is
# the gate core::base uses for the same reason, and nothing in the WebAssembly subset below has a
# mutex or a thread in it.
set(_coreCppNetPublicLibs core::net_types core::async core::platform)
if(CORE_CPP_USE_THREADS)
    list(APPEND _coreCppNetPublicLibs Threads::Threads)
endif()

# What builds under single-threaded WebAssembly: the event loop, the backend interface, the
# host-driven backend and the pure logic behind them. There is no thread to block and no descriptor
# to poll there, so a loop is PUMPED by the browser through an IHostScheduler; readiness arrives
# through the browser's own APIs and reaches the loop as a post. `run()` and `blockOn()` are
# COMPILED here rather than removed -- they assert at runtime instead, because the browser reaches
# them through a consumer's mistake and a symbol simply absent there fails at link time in somebody
# else's build. The test doubles are header-only and need no row here.
set(_coreCppNetSubset
    DeadlineTimer.cpp
    EventLoop.cpp
    HostDrivenBackend.cpp
    InterruptibleSleep.cpp
    IoAwaitable.cpp
    detail/CompletionHook.hpp
    detail/ParkId.hpp
    detail/ParkTable.hpp
    detail/ReadyBatch.hpp
    detail/RingQueue.hpp
    detail/ScopeGuard.hpp
    detail/WaitTimeout.hpp
    detail/WorkerIdentity.hpp)
# The timer cases join the subset binary rather than the module's own: they drive a TestLoop and a
# ManualClock and touch no descriptor, so they are cases the browser can run -- and the browser is
# what Task B5's deadlines exist for.
set(_coreCppNetSubsetTests
    CompletionClaim_test.cpp
    DeadlineTimer_test.cpp
    HostDrivenBackend_test.cpp
    HostDrivenLoop_test.cpp
    InterruptibleSleep_test.cpp
    IoBackend_test.cpp
    LoopExecutorContext_test.cpp
    ReadinessQueue_test.cpp
    SleepUntil_test.cpp
    Timers_test.cpp
    detail/ParkTable_test.cpp
    detail/RingQueue_test.cpp
    testing/ScriptedBackend_test.cpp)
#
# Exactly one DefaultBackend.cpp is compiled, and it is the whole of how a platform picks its
# backend — no `#ifdef` chooses one (Ruling R40). It cannot simply be listed per directory the way
# the backends themselves are: SOURCES_POSIX and SOURCES_LINUX BOTH apply on Linux, so a
# posix/DefaultBackend.cpp and a linux/DefaultBackend.cpp would both define makeBackend and collide
# at link time. The platform is therefore asked once, here.
if(EMSCRIPTEN)
    set(_coreCppNetDefaultBackend emscripten/DefaultBackend.cpp)
elseif(WIN32)
    set(_coreCppNetDefaultBackend windows/DefaultBackend.cpp)
elseif(LINUX)
    set(_coreCppNetDefaultBackend linux/DefaultBackend.cpp)
elseif(CORE_CPP_PLATFORM_BSD)
    set(_coreCppNetDefaultBackend bsd/DefaultBackend.cpp)
else()
    set(_coreCppNetDefaultBackend posix/DefaultBackend.cpp)
endif()

# The `else()` arm above is the only platform branch in this repository that NO configuration we
# run takes: it needs a POSIX platform that is not Linux, not a BSD, not Apple, not Windows and not
# Emscripten -- Solaris, AIX, illumos. So `posix/DefaultBackend.cpp` was compiled by nothing at all,
# and a rename or a dropped include would have broken it silently until somebody ported core-cpp.
# Everywhere else, it is parsed for diagnostics only. Read
# core_cpp_add_unbuilt_source_check()'s comment before trusting a green result: it proves the file
# still PARSES against the host's headers, not that it is correct on the platform that takes it.
if(NOT _coreCppNetDefaultBackend STREQUAL "posix/DefaultBackend.cpp")
    core_cpp_add_unbuilt_source_check(net-default-backend
        SOURCES posix/DefaultBackend.cpp
        USAGE core::net_types core::async core::platform)
endif()

# The module's root holds only platform-independent code. What one platform needs lives in posix/,
# linux/ (epoll), bsd/ (kqueue, for Apple and the BSDs) and windows/, and the source lists below
# choose it, so no file there guards itself with an #ifdef of its platform. contour's
# PollEventSource.cpp was split along its #ifdef into posix/PollBackend.cpp and a Windows half,
# the WFMO backend, which 0.5.0 removed (core-cpp#6); the socket pair of
# testing/InMemoryTransport.cpp is split the same way, into testing/posix/ and testing/windows/. One exception remains: BackendParity_test.cpp keeps three
# POSIX-only cases (a closed descriptor's registration, descriptor exhaustion, a muted registration
# whose peer hangs up), and the includes they need, under #ifndef _WIN32.
core_cpp_add_module(net KIND STATIC
    HEADERS
        AsyncBufferedReader.hpp
        BlockingConnector.hpp
        BlockingSocket.hpp
        ConnectFlow.hpp
        DeadlineTimer.hpp
        Diagnostics.hpp
        EventLoop.hpp
        HealthProbe.hpp
        HostDrivenBackend.hpp
        HttpServer.hpp
        IAdmissionControl.hpp
        IAsyncAddressResolver.hpp
        ICompletionPort.hpp
        IConnector.hpp
        IDatagramSocket.hpp
        IHostScheduler.hpp
        IListener.hpp
        InterruptibleSleep.hpp
        IoAwaitable.hpp
        IoBackend.hpp
        ISocket.hpp
        ITlsContext.hpp
        KeepAlive.hpp
        LingeringClose.hpp
        PlatformLoop.hpp
        ReadinessDial.hpp
        SharedPortDatagram.hpp
        SocketAddress.hpp
        SocketBuffers.hpp
        SocketContract.hpp
        SocketDeadline.hpp
        SleepUntil.hpp
        Sockets.hpp
        SplitSocket.hpp
        TcpClient.hpp
        ThreadedAddressResolver.hpp
        UdpSocket.hpp
        WithTimeout.hpp
        WriteQueue.hpp
        detail/CompletionHook.hpp
        detail/ParkId.hpp
        detail/ParkTable.hpp
        detail/ReadinessSlot.hpp
        detail/ReadyBatch.hpp
        detail/RingQueue.hpp
        detail/ScopeGuard.hpp
        detail/StreamSocketOptions.hpp
        detail/WorkerIdentity.hpp
        testing/BackendMatrix.hpp
        testing/CoroTestSupport.hpp
        testing/DatagramPayload.hpp
        testing/InMemoryDatagram.hpp
        testing/InMemorySocket.hpp
        testing/InMemoryTransport.hpp
        testing/ManualHostScheduler.hpp
        testing/NullBackend.hpp
        testing/ParkingReadableSocket.hpp
        testing/RawSockets.hpp
        testing/ScriptedBackend.hpp
        testing/SocketDecorator.hpp
        testing/TestLoop.hpp
    SOURCES
        ${_coreCppNetSubset}
        AsyncBufferedReader.cpp
        BlockingConnector.cpp
        BlockingSocket.cpp
        ConnectFlow.cpp
        Connector.cpp
        Diagnostics.cpp
        HealthProbe.cpp
        HttpServer.cpp
        ISocket.cpp
        LingeringClose.cpp
        ReadinessDial.cpp
        SharedPortDatagram.cpp
        SocketAddress.cpp
        SocketContract.cpp
        Sockets.cpp
        TcpClient.cpp
        ThreadedAddressResolver.cpp
        WriteQueue.cpp
        testing/InMemoryDatagram.cpp
        testing/InMemorySocket.cpp
        ${_coreCppNetDefaultBackend}
        detail/BlockingPrimitives.hpp
        detail/DatagramAddressing.hpp
        detail/DatagramReceiveBuffer.hpp
        detail/DialPrimitives.hpp
        detail/PeerAddress.hpp
        detail/SocketErrors.hpp
        detail/WakeupChannel.hpp
    SOURCES_POSIX
        detail/WouldBlock.hpp
        posix/AcceptLoop.cpp
        posix/DialPrimitives.cpp
        posix/AcceptLoop.hpp
        posix/BlockingPrimitives.cpp
        posix/FdUtils.hpp
        posix/PollBackend.cpp
        posix/PollBackend.hpp
        posix/PosixListener.cpp
        posix/PosixListener.hpp
        posix/PosixSocket.cpp
        posix/PosixSocket.hpp
        posix/SocketErrors.cpp
        posix/SocketsPosix.cpp
        posix/StreamSocketOptions.cpp
        posix/UdpSocket.cpp
        posix/UnixListener.cpp
        posix/UnixListener.hpp
        testing/posix/InMemoryTransport.cpp
        testing/posix/RawSockets.cpp
    SOURCES_LINUX
        linux/EpollBackend.cpp
        linux/EpollBackend.hpp
    SOURCES_BSD
        bsd/KqueueBackend.cpp
        bsd/KqueueBackend.hpp
    SOURCES_WINDOWS
        testing/windows/InMemoryTransport.cpp
        testing/windows/RawSockets.cpp
        windows/BlockingPrimitives.cpp
        windows/DialPrimitives.cpp
        windows/InvalidSocket.hpp
        windows/IocpBackend.cpp
        windows/IocpBackend.hpp
        windows/IocpDial.cpp
        windows/IocpOperation.hpp
        windows/IocpSocket.cpp
        windows/IocpSocket.hpp
        windows/NetworkEvents.cpp
        windows/NetworkEvents.hpp
        windows/SocketErrors.cpp
        windows/SocketsWin32.cpp
        windows/StreamSocketOptions.cpp
        windows/UdpSocket.cpp
        windows/UnixSocketPath.cpp
        windows/UnixSocketPath.hpp
        windows/WaitCompletionPacket.cpp
        windows/WaitCompletionPacket.hpp
        windows/WindowsLoopback.cpp
        windows/WindowsLoopback.hpp
        windows/WinsockError.hpp
    SOURCES_EMSCRIPTEN
        ${_coreCppNetSubset}
        ${_coreCppNetDefaultBackend}
        emscripten/EmscriptenHostScheduler.cpp
        emscripten/EmscriptenHostScheduler.hpp
    PUBLIC_LIBS ${_coreCppNetPublicLibs}
    PRIVATE_LIBS ${_coreCppNetPrivateLibs})

# detail/ScopeGuard.hpp is listed with core::net, not here: net's own tests use it too, and a
# header belongs with the target every user of it links.
#
# testing/StrictTlsPeer is the TLS layer's test double, public like every double under testing/:
# OpenSSL driven by hand, which a consumer can hold its own TLS code against as the tests here hold
# this layer. It is the second of the two units `core-cpp.openssl-seam` lets include OpenSSL
# (tests/cmake/check-openssl-seam.cmake), and its header names no OpenSSL type.
core_cpp_add_module(net_tls KIND STATIC
    HEADERS
        Tls.hpp
        testing/StrictTlsPeer.hpp
    SOURCES
        Tls.cpp
        testing/StrictTlsPeer.cpp
    PUBLIC_LIBS core::net
    PRIVATE_LIBS OpenSSL::SSL OpenSSL::Crypto)

# The error vocabulary alone, in every build: under Emscripten this is what proves net_types builds.
core_cpp_add_test(net NAME net_types SOURCES NetError_test.cpp)

# Nearly every case moves bytes over a socket: a real one, a socketpair, or on Windows the loopback
# TCP pair that backs SystemPipe and the in-memory transport alike, hence the label on the binary.
# The backend contract, and the host-driven backend over testing::ManualHostScheduler: a binary of
# its own because it is the part that builds everywhere, Emscripten included, and because none of it
# moves a byte over a socket — so it carries neither the `loopback` label nor the timeout the cases
# that do need.
core_cpp_add_test(net NAME net_backend
    SOURCES ${_coreCppNetSubsetTests}
    SOURCES_EMSCRIPTEN ${_coreCppNetSubsetTests})

# What a drain-step callback costs in allocations: a binary of its own because it replaces the
# global allocation functions, which would reach every case linked beside it. Not under a
# sanitizer, whose runtime defines those functions itself (the link refuses a second definition)
# and whose allocator is not the one a Release program counts against.
if(NOT CORE_CPP_SANITIZERS)
    core_cpp_add_test(net NAME net_alloc
        SOURCES CallbackAllocation_test.cpp ../testing/ReplacedGlobalAllocation.cpp
        SOURCES_EMSCRIPTEN CallbackAllocation_test.cpp ../testing/ReplacedGlobalAllocation.cpp)
endif()

# Everything that needs a loop, a socket or a descriptor, and so nothing that builds under
# Emscripten yet. Asked here rather than left to an empty SOURCES_EMSCRIPTEN, which
# core_cpp_add_test reads as a mistake and refuses: a binary named after the module follows the
# module's row, and that row now says the module has a WebAssembly subset. The subset's own cases
# are the net_backend binary above.
if(NOT EMSCRIPTEN)
core_cpp_add_test(net
    SOURCES
        AcceptedHalfClose_test.cpp
        AdoptSocket_test.cpp
        AsyncBufferedReader_test.cpp
        BackendParity_test.cpp
        BlockingConnector_test.cpp
        BlockingSocket_test.cpp
        CancelRead_test.cpp
        ClockRefresh_test.cpp
        ClosedParkIdle_test.cpp
        CloseResumesThroughLoop_test.cpp
        ConnectFlow_test.cpp
        Connector_test.cpp
        EventLoop_test.cpp
        HealthProbe_test.cpp
        HttpServer_test.cpp
        IAdmissionControl_test.cpp
        IoAwaitable_test.cpp
        LingeringClose_test.cpp
        LoopTeardown_test.cpp
        ReadinessDial_test.cpp
        SharedPortDatagram_test.cpp
        SocketAddress_test.cpp
        SocketClosedStates_test.cpp
        SocketDeadline_test.cpp
        Socket_test.cpp
        StreamSocketOptions_test.cpp
        TcpClient_test.cpp
        ThreadedAddressResolver_test.cpp
        UdpSocket_test.cpp
        WaitReadable_test.cpp
        WithTimeout_test.cpp
        WriteQueue_test.cpp
        testing/InMemoryDatagram_test.cpp
        testing/InMemorySocket_test.cpp
        testing/ParkingReadableSocket_test.cpp
        testing/TestLoop_test.cpp
    # POSIX-only, each for its own reason. `ReactorSocket_test` names PosixSocket directly, and
    # `SocketRegistration_test` counts what PosixSocket's kept registration costs the backend.
    # `ParkReuse_test` drives the loop's park interface with raw `socketpair` descriptors, and
    # it is POSIX-only because `PosixSocket` is the only filer of `RegistrationLifetime::UntilClosed`
    # parks, the one kind the loop keeps resident: a Windows socket that files them owes these cases.
    # `CompletionCost_test` is a benchmark that reports USER time apart from system time, which it
    # reads with `getrusage`.
    # `SocketDecorator_test` needs a WRITE that parks, and assumes no send buffer takes 8MiB: on
    # Windows a non-blocking send takes all of it at once (measured, Task B7b), so its third
    # section would retire nothing. `CancelRead_test` left this list with Task B7b, when `IocpSocket`
    # implemented `cancelRead`, and it runs on every backend.
    #
    # `posix/AdoptListener_test` is the exception to that sentence: it is NOT a POSIX-only
    # BEHAVIOUR, it is a POSIX-only way to ARRANGE one. `adoptListener` exists for a listening
    # handle this library did not make, so the case has to make one with the platform's own API --
    # `::socket`/`::bind`/`::listen` here, the Winsock spelling in
    # `windows/AdoptListener_test.cpp` -- and the two files assert the same three things. A
    # portable file would have had to carry both spellings behind an `#ifdef`, which is the shape
    # Ruling R40 keeps out of this module.
    SOURCES_POSIX
        CompletionCost_test.cpp
        ParkReuse_test.cpp
        ReactorSocket_test.cpp
        SocketDecorator_test.cpp
        SocketRegistration_test.cpp
        posix/AdoptListener_test.cpp
        posix/FdPassing_test.cpp
        posix/PortSharing_test.cpp
        posix/SocketErrors_test.cpp
        posix/UnixSocket_test.cpp
        posix/WaitReadableClose_test.cpp
    SOURCES_WINDOWS
        windows/AdoptListener_test.cpp
        windows/IocpBackend_test.cpp
        windows/IocpDial_test.cpp
        windows/IocpSocket_test.cpp
        windows/NetworkEvents_test.cpp
        windows/PortSharing_test.cpp
        windows/SocketErrors_test.cpp
        windows/UnixListener_test.cpp
    LABELS loopback
    # A reactor defect does not fail a case here, it HANGS it: a lost readiness wake-up parks a
    # flow with nothing left to resume it. Each such case bounds its own wait and says what it
    # waited for; this is the backstop for one that forgets, and reports in two minutes rather
    # than ctest's default 1500 seconds. The binary runs in about 1.5s locally and under 10s on
    # the slowest CI runner, so the margin is two orders of magnitude.
    TIMEOUT 120)
endif()

# TIMEOUT for the same reason as above, and with more cause: two of these cases say in their own
# comments that a regression does not fail them, it hangs them.
#
# The TLS cases are held against `testing::StrictTlsPeer` rather than only against another TLS
# socket, which would agree with itself. The binary links no OpenSSL of its own: the peer is part of
# core::net_tls, and OpenSSL stays PRIVATE to it.
core_cpp_add_test(net NAME net_tls
    SOURCES
        TlsContext_test.cpp
        TlsLifetime_test.cpp
        TlsCancelRead_test.cpp
        TlsSocket_test.cpp
        Tls_test.cpp
    LABELS loopback
    TIMEOUT 120)

# `EventLoop::run()` and `EventLoop::blockOn()` on a host-driven loop are precondition violations,
# and an assertion cannot be asserted from inside a Catch case: it aborts the process, which ends
# the binary rather than the case. Each mode is therefore its own process, and each must FAIL.
# Not under Emscripten, where a bare executable is a node invocation that core_cpp_add_test's
# wrapper exists to arrange; the refusal is compiled there (the net_backend binary instantiates
# both), and driving it under node is what Task B5's wasm smoke leg adds.
# CORE_CPP_TESTING as well as the platform, and the second half is what a consumer needs: every
# other test here goes through core_cpp_add_test, which returns early without it, so a CPM consumer
# builds core-cpp and registers none of core-cpp's tests. A bare add_test() bypasses that gate --
# the test lands in the CONSUMER's ctest naming an executable their build never made, and reports
# "Not Run" in somebody else's suite.
if(CORE_CPP_TESTING AND NOT EMSCRIPTEN)
    add_executable(core-cpp-hostdriven-canary HostDrivenCanary.cpp)
    # core::testing_dialogs, and it is load-bearing rather than tidiness: a failed assert() in an
    # MSVC Debug build opens a message box and WAITS, so the canary times out instead of failing,
    # which reads as a hang rather than as the refusal firing. Linking this installs the
    # suppression at startup, the same way tests/WindowsDialogCanary.cpp does. It is an object
    # library on every platform and a no-op off Windows.
    target_link_libraries(core-cpp-hostdriven-canary PRIVATE core::net core::testing_dialogs)
    core_cpp_apply_toolchain(core-cpp-hostdriven-canary)
    foreach(mode IN ITEMS run blockOn closedPark)
        add_test(NAME core-cpp.hostdriven-canary.${mode}
                 COMMAND core-cpp-hostdriven-canary ${mode})
        # **A canary is judged by a POSITIVE match, not by WILL_FAIL.** WILL_FAIL inverts ANY
        # non-zero exit, so a run that never reached its assertion scored as a pass: measured,
        # a deliberately bogus mode exits 2 and ctest called it Passed. A negative alternation
        # only narrows that -- it can exclude the paths you enumerated, and the paths that print
        # NOTHING are invisible to it. `PASS_REGULAR_EXPRESSION` inverts the burden: the marker
        # printed immediately before the forbidden operation must appear, so reaching the
        # mechanism is what passes and every silent exit fails.
        #
        # Measured, because CMake documents none of it: SKIP_RETURN_CODE still wins over both
        # regexes (so a Release leg, where assertions are compiled out, abstains rather than
        # fails); FAIL_ beats PASS_ when both match, which is what makes the "continued" guard
        # work even though the marker was printed; and a run with no output at all fails with
        # "Required regular expression not found".
        #
        # PASS_REGULAR_EXPRESSION makes ctest IGNORE the exit code, so it REPLACES WILL_FAIL
        # rather than accompanying it -- together they would invert the regex verdict and the
        # test would pass when the marker was absent.
        #
        # **The `onAbort` handler in the canary is load-bearing for all of this**, not tidiness:
        # it turns SIGABRT into `_Exit(1)`, and both properties are documented as unable to
        # override a signal. Delete it as redundant and every scheme here breaks silently.
        #
        # The FAIL_ entry is the one that must be here: those two lines are printed AFTER the
        # marker, on the path where the refusal did not fire, so PASS_ alone would accept them.
        set_tests_properties(core-cpp.hostdriven-canary.${mode} PROPERTIES
            PASS_REGULAR_EXPRESSION "${mode}: about to"
            TIMEOUT 60
            SKIP_RETURN_CODE ${CORE_CPP_SKIP_EXIT_CODE}
            FAIL_REGULAR_EXPRESSION "was accepted;returned on a host-driven loop"
            LABELS "core-cpp;net;canary")
    endforeach()
    # `closedPark` is judged on the ASSERTION'S OWN TEXT, as the loop-affinity canaries below are:
    # its marker is printed before `addTimer`, and an abort anywhere on the `registerPark` path
    # after it -- another assertion than the one this mode exists for -- would match the marker.
    # The colon is what keeps it apart from the canary's own "... pending returned" line.
    set_tests_properties(core-cpp.hostdriven-canary.closedPark PROPERTIES
        PASS_REGULAR_EXPRESSION "armHostWake with a closed park pending: a host-driven backend")

    # Every loop-thread-only member of EventLoop, called from a second thread while the loop runs on
    # its own -- fastcached's reactor-teardown-gate (0708dd54) in this tree's shape. One mode per
    # member, and the PASS expression is the ASSERTION'S OWN TEXT, naming that member, rather than a
    # marker printed before the call: a process that died of something else on its way there (a
    # race's segfault, a failed thread spawn) prints no such text, and a mode that reached another
    # member's guard names the other member. Upstream's gate kept exactly those outcomes apart, and
    # measured that "died" is not "the guard refused". Every message is matched on its FIRST string
    # literal only, because the compilers print the expression as written and a message split across
    # two literals is printed with the quotes between them.
    add_executable(core-cpp-loop-affinity-canary LoopAffinityCanary.cpp)
    target_link_libraries(core-cpp-loop-affinity-canary PRIVATE core::net core::testing_dialogs)
    core_cpp_apply_toolchain(core-cpp-loop-affinity-canary)
    set(_coreCppAffinityModes
        "destroy|an EventLoop must be destroyed on its own worker thread"
        "runOnce|an EventLoop turn was entered from a second thread while another is driving")
    foreach(member IN ITEMS cancelPending requestStop spawn addTimer cancelTimer resumeSoon registerPark
                            unregisterPark wakeReasonOf notifyHandleClosing)
        list(APPEND _coreCppAffinityModes
             "${member}|EventLoop::${member} from a second thread while another is driving")
    endforeach()
    foreach(row IN LISTS _coreCppAffinityModes)
        string(FIND "${row}" "|" bar)
        string(SUBSTRING "${row}" 0 ${bar} mode)
        math(EXPR bar "${bar} + 1")
        string(SUBSTRING "${row}" ${bar} -1 refusal)
        add_test(NAME core-cpp.loop-affinity-canary.${mode}
                 COMMAND core-cpp-loop-affinity-canary ${mode})
        set_tests_properties(core-cpp.loop-affinity-canary.${mode} PROPERTIES
            PASS_REGULAR_EXPRESSION "${refusal}"
            FAIL_REGULAR_EXPRESSION "was accepted;unknown mode;usage:;no default backend"
            TIMEOUT 60
            SKIP_RETURN_CODE ${CORE_CPP_SKIP_EXIT_CODE}
            LABELS "core-cpp;net;canary")
    endforeach()
endif()

# The IOCP backend's two refusals, each its own process for the reason above: an
# assertion aborts, which ends the binary rather than the case. G1 (a second thread
# dequeuing the port) and G4 (a handle associated with one port twice) are the two a
# completion port would otherwise let you commit silently -- IOCP is DESIGNED to be
# drained by many threads, and the kernel's refusal of a second association is
# indistinguishable from six ordinary mistakes.
if(CORE_CPP_TESTING AND WIN32)
    add_executable(core-cpp-iocp-canary windows/IocpCanary.cpp)
    # core::testing_dialogs for the same load-bearing reason the host-driven canary
    # links it: a failed assert() in an MSVC Debug build opens a message box and WAITS,
    # so the canary times out instead of failing, which reads as a hang rather than as
    # the refusal firing.
    target_link_libraries(core-cpp-iocp-canary PRIVATE core::net core::testing_dialogs)
    core_cpp_apply_toolchain(core-cpp-iocp-canary)
    foreach(guarantee IN ITEMS g1 g4)
        add_test(NAME core-cpp.iocp-canary.${guarantee}
                 COMMAND core-cpp-iocp-canary ${guarantee})
        # **A canary is judged by a POSITIVE match, not by WILL_FAIL.** WILL_FAIL inverts ANY
        # non-zero exit, so a run that never reached its assertion scored as a pass: measured,
        # a deliberately bogus mode exits 2 and ctest called it Passed. A negative alternation
        # only narrows that -- it can exclude the paths you enumerated, and the paths that print
        # NOTHING are invisible to it. `PASS_REGULAR_EXPRESSION` inverts the burden: the marker
        # printed immediately before the forbidden operation must appear, so reaching the
        # mechanism is what passes and every silent exit fails.
        #
        # Measured, because CMake documents none of it: SKIP_RETURN_CODE still wins over both
        # regexes (so a Release leg, where assertions are compiled out, abstains rather than
        # fails); FAIL_ beats PASS_ when both match, which is what makes the "continued" guard
        # work even though the marker was printed; and a run with no output at all fails with
        # "Required regular expression not found".
        #
        # PASS_REGULAR_EXPRESSION makes ctest IGNORE the exit code, so it REPLACES WILL_FAIL
        # rather than accompanying it -- together they would invert the regex verdict and the
        # test would pass when the marker was absent.
        #
        # **The `onAbort` handler in the canary is load-bearing for all of this**, not tidiness:
        # it turns SIGABRT into `_Exit(1)`, and both properties are documented as unable to
        # override a signal. Delete it as redundant and every scheme here breaks silently.
        #
        # The marker is keyed to ${guarantee} because one shared string would make g4 fail: it
        # never prints a g1 marker. `nothing refused it` is printed by both not-fired paths, so
        # one entry covers both -- and it is the only negative entry needed here, because this
        # binary's other exits (an unknown mode, a backend offering no port, a socket it could
        # not associate) print no marker and are already failed by PASS_. That includes the path
        # that defeats any alternation: IocpBackend's constructor throwing exits 1 with EMPTY
        # stderr, because the testing_dialogs suppression that makes this canary usable also
        # removes the CRT's abort text.
        set_tests_properties(core-cpp.iocp-canary.${guarantee} PROPERTIES
            PASS_REGULAR_EXPRESSION "${guarantee}: about to"
            FAIL_REGULAR_EXPRESSION "nothing refused it"
            TIMEOUT 60
            SKIP_RETURN_CODE ${CORE_CPP_SKIP_EXIT_CODE}
            LABELS "core-cpp;net;canary")
    endforeach()
endif()

# The in-memory FAKE's two slot guards, watched refusing: a double that let a second read or write
# arm over a parked one would be more permissive than the sockets it stands for, and an assertion
# aborts the process, so SocketClosedStates_test cannot see it. Judged on the mode-named marker
# printed immediately before the guarded call, never WILL_FAIL -- the block below says why. On every
# platform, Windows included, because the fake has no platform; core::testing_dialogs so an MSVC
# Debug assert fails instead of opening a dialog and waiting.
if(CORE_CPP_TESTING AND NOT EMSCRIPTEN)
    add_executable(core-cpp-inmemory-socket-canary testing/InMemorySocketCanary.cpp)
    target_link_libraries(core-cpp-inmemory-socket-canary PRIVATE core::net core::testing_dialogs)
    core_cpp_apply_toolchain(core-cpp-inmemory-socket-canary)
    foreach(guard IN ITEMS fake-read-slot fake-write-slot)
        add_test(NAME core-cpp.inmemory-socket-canary.${guard}
                 COMMAND core-cpp-inmemory-socket-canary ${guard})
        set_tests_properties(core-cpp.inmemory-socket-canary.${guard} PROPERTIES
            PASS_REGULAR_EXPRESSION "${guard}: reached the guarded call"
            FAIL_REGULAR_EXPRESSION "SURVIVED;unknown mode;usage:"
            TIMEOUT 60
            SKIP_RETURN_CODE ${CORE_CPP_SKIP_EXIT_CODE}
            LABELS "core-cpp;net;canary")
    endforeach()
endif()

# The three socket-contract guards, watched refusing against a REAL socket.
#
# **Registered on MARKERS rather than WILL_FAIL**, and the program's own header says why at length:
# WILL_FAIL inverts any non-zero exit, so a canary that dies before reaching the guarded call reads
# exactly like one whose assertion fired -- and an uncaught throw reaches abort() with stderr empty,
# which no failure-string alternation can catch. The PASS expression is the marker printed
# immediately BEFORE the forbidden operation, which is what proves the process got that far; the
# FAIL expression is the marker printed after it plus every early-exit text. ctest checks FAIL
# first, which is what lets the pass marker sit on both paths.
#
# The PASS expression names the MODE, because a marker shared by all three would be printed
# by whichever guard the process actually reached: a mode that fell into the wrong branch
# would pass on another guard's marker.
#
# The slot guards end the process in every build, so every mode but empty-read-buffer runs on the
# Release legs too; that one is still an assertion, and SKIP_RETURN_CODE, evaluated ahead of both
# expressions, lets a Release build abstain rather than fail for want of a marker NDEBUG never
# prints. watch-read-slot and watch-write-slot are the loop's half of the rule: two parks on one
# handle's lifetime registration, filed through EventLoop::registerPark.
#
# Windows too since Task B7b, where the default backend's socket is `IocpSocket`. The two write
# modes need a write that PARKS, and a Windows non-blocking send takes 8MiB at once (measured), so
# they keep writing until one stays pending (at most 256MiB) and SKIP with a stderr reason if none
# ever does -- never a pass on a write that did not park.
if(CORE_CPP_TESTING AND NOT EMSCRIPTEN)
    add_executable(core-cpp-socket-contract-canary SocketContractCanary.cpp)
    target_link_libraries(core-cpp-socket-contract-canary PRIVATE core::net core::testing_dialogs)
    core_cpp_apply_toolchain(core-cpp-socket-contract-canary)
    foreach(guard IN ITEMS read-slot write-slot write-slot-inline watch-read-slot watch-write-slot
                           empty-read-buffer)
        add_test(NAME core-cpp.socket-contract-canary.${guard}
                 COMMAND core-cpp-socket-contract-canary ${guard})
        set_tests_properties(core-cpp.socket-contract-canary.${guard} PROPERTIES
            PASS_REGULAR_EXPRESSION "${guard}: reached the guarded call"
            FAIL_REGULAR_EXPRESSION "SURVIVED;unknown mode;usage:;no default backend;could not make a socket pair;did not park"
            TIMEOUT 60
            SKIP_RETURN_CODE ${CORE_CPP_SKIP_EXIT_CODE}
            LABELS "core-cpp;net;canary")
    endforeach()
endif()
