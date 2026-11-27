# Review ci5-macos (saved by controller; reviewer read-only)
Spec: pass (process deviation: new commits). Quality: pass. No Critical/Important.
Diagnosis judged sound from XNU source (uipc_shutdown -> sorflush SB_DROP; uipc_send checks only own SS_CANTSENDMORE; filt_soread EV_EOF -> POLLHUP). Peer SHUT_RD alone gives no POLLHUP.
1. Minor Channel_posix.cpp:262-266 — check asks "did my receive side end", relies on every end shutting down SHUT_RDWR (only call: :574). Document the invariant.
2. Minor Channel_posix.cpp:339/320 — trySend now polls twice per chunk on macOS; merge into one poll POLLIN|POLLOUT returning Closed on POLLHUP; reword :321 comment.
3. Minor Channel_posix.cpp:532 — sendWithHandle's new check untested; add a case (send with handle after peer shutdown -> Closed).
4. Minor process — new commits accepted; fold 3d8a32406 into a14efb900 at the pre-merge squash.
5. Note (not this diff) — XNU soshutdownlock_final returns ENOTCONN early when receive side already ended, so a thread blocked in ::send on this end may not be woken. Follow-up: macOS test when a Mac is available.
