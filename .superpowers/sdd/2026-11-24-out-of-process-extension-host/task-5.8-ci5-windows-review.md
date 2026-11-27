# Review ci5-windows (reviewer was read-only; saved by controller)
Spec: PASS. Quality: PASS WITH FINDINGS.
Important
1. Channel_test.cpp:208-223 — baseline taken before std::async starts the receiver thread; thread/threadpool handles can make most==before fail with the fix. Start thread parked on a promise, take baseline, then release into receiveSome.
2. Channel_win32.cpp:195,202,253 — CreateEventW failure swallowed in Native ctor; end then fails every transfer with Io, no reason. Check both events in adopt() and makeChannelPair() (~300, ~425) and refuse there (std::expected).
Minor
3. Channel.hpp:11 — reused receive event needs one receiver at a time per end; document it (TransferDirection comment claims Channel.hpp covers both).
4. Channel_win32.cpp:65 — OverlappedEvent comment says "closed with it" (the operation); now closed with the channel end.
5. LaunchSpelling_win32.cpp:115 — require profile.is_absolute().
6. LaunchProbe.cpp:587 — getsockopt result ignored; report its failure.
7. HostProcess.cpp:204 — held list claims every handle; Windows channel ends now keep two events each, unnamed. Name them or narrow the contract.
8. DescriptorRights.cpp:34 — StopEventRights row never enforced on Windows; say so.

## Re-review round 1 (scoped)
2-8 addressed (#7 narrowed wording accepted). #1 PARTIAL (Important): std::async on MSVC submits to the threadpool, whose worker/handles appear asynchronously after the call; the baseline can still precede them. Fix: a second promise `started` set by the lambda before `released.wait()`; the test waits on it before taking `before`.
New minor: Channel_test.cpp ~208-222 — `go` declared before `received`, so on a REQUIRE failure between async start and go.set_value() the future's destructor blocks forever. Declare `received` before `go`, or set go on all paths via a scope guard.
