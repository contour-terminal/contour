# Task 5.8 — CI round 5, macOS: report

Status: DONE_WITH_CONCERNS. There is no Mac, so the macOS branch was compiled and run on Linux with its constants
forced, not on macOS itself.

Base: 9c87b6d5d (round-3 minors and the Windows round-5 commits; both left as they were). Two new commits, not
pushed:

| Commit | Subject |
|---|---|
| e135d799f | test(vtextension): a failed channel check prints the error it got |
| 3d8a32406 | fix(vtextension): a send to a channel whose peer shut down fails on macOS |

Why new commits rather than fixups:
- The broken contract ("the peer's sends return Closed") and its test came in eb9d2aab3, and the POSIX channel
  itself came in a14efb900.
- Both lie about 150 commits back in history that is already pushed.
- Folding into them would rewrite most of the branch, so the fix is a commit of its own, as in round 4.

## Root cause (both failures share one)

Failing checks:
- Channel.shutdownEndsThePeersStream, Channel_test.cpp:126: `helper.send(1024 bytes)` after `parent.shutdown()`.
- Channel.trySendNeverBlocks, Channel_test.cpp:270: `parent.trySend(chunk)` after `helper.shutdown()`.

The failing pattern: one end calls `shutdown(SHUT_RDWR)` without closing, and then its **peer** sends.
`Channel::shutdown()` promises those sends return Closed (Channel.hpp).

On XNU (apple-oss-distributions/xnu, bsd/kern/uipc_usrreq.c and uipc_socket.c):
- `uipc_shutdown()` calls `socantsendmore(so)`, then `unp_shutdown()`, which calls `socantrcvmore(so2)` on the
  peer. The peer's *receiving* side is marked ended. Its sending side is not.
- `sorflush()` on the shutting end sets `SB_DROP` on that end's receive buffer.
- `uipc_send()` (SOCK_STREAM) checks only the sender's own `SS_CANTSENDMORE` (EPIPE), twice. It never checks the
  peer's `SS_CANTRCVMORE` or `SB_DROP`. It then calls `sbappend()` on the peer's receive buffer, which drops the bytes
  and returns no error.

So on macOS the send succeeds: `send()` returns `{}` and `trySend()` returns a byte count. Catch2 printed `{?} == {?}`.

Linux refuses the same send in `unix_stream_sendmsg()`, which checks the other end's `RCV_SHUTDOWN` and fails with
EPIPE (mapped to Closed). FreeBSD's CI is green on the same tests.

What macOS does tell the sender is that its own receiving side ended:
- XNU's poll is built on kevent, and its `poll_callback()` maps the read filter's EV_EOF to POLLHUP (sys_generic.c).
- `filt_soread` raises EV_EOF on SS_CANTRCVMORE, even with bytes left to read.

### Ruled out
- **SO_NOSIGPIPE:** a SIGPIPE would have killed the run, and it did not.
- **The errno classification:** EPIPE, ECONNRESET, ENOTCONN and ESHUTDOWN all map to Closed already. No errno is
  returned at all.
- **The round-4 poll-before-send path (trySend only):** shutdownEndsThePeersStream fails through plain `send()`,
  which does not take that path.

## Fix (Channel_posix.cpp)

- **New per-platform table row.** `SendToEndedPeer` is `{Fails, Dropped}`: `Dropped` on `__APPLE__`, `Fails`
  elsewhere, next to `UnclaimedDescriptors` and `DontWaitSend`.
- **New check.** `refuseSendToEndedPeer(int)` does a `poll(POLLIN, 0)` (retrying on EINTR) and returns Closed on
  POLLHUP, or Io if the poll fails. It is a no-op where the send fails by itself.
- **Where it runs:** every iteration of `send()` and `trySend()`, and before `sendWithHandle()`'s `sendmsg()`.
- **It also covers this end's own shutdown,** which raises POLLHUP the same way (Closed, as the contract says), and a
  peer that closed.
- **Cost on macOS:** one extra non-blocking poll per send call iteration. Linux and FreeBSD are unchanged.
- **Race:** a peer that shuts down after the check but before the send loses those bytes, as it would any bytes in
  flight. The contract only concerns sends made after the shutdown returned.
- **Docs:** the file-header comment now carries the reasoning.

## StringMaker (test only)

- New `transport/testing/ChannelStrings.hpp`: one table names every ChannelError (`nameOf()`).
- Catch2 now prints three types:
  - `ChannelError`;
  - `std::expected<T, ChannelError>`, as `value <v>` or `value` for void, and `error <name>`;
  - `std::unexpected<ChannelError>`, as `error <name>`.
- Channel_test.cpp includes the header.
- Checked with a temporary failing check, since reverted: it printed `error Closed == error Io` instead of
  `{?} == {?}`.

## Unasserted probe rows (AF_ROUTE, AF_SYSTEM, SIOCGIFCONF)

The log does not show them:
- In run 45826489690, `vtextension_process_test` passed.
- ctest prints a test's output only on failure, and `CTEST_OUTPUT_ON_FAILURE` is the only output setting in this job.
- So the WARN lines for socket-route, socket-system, list-interfaces (and map-jit) never reached the log. The only
  `vtextension_process_test` lines in it are its compile steps, near line 6800.

To see them, a later round could run `vtextension_process_test "Sandbox.forbiddenOperationsFailFromAnExecutor"` as
its own step, or with `-s`, or give ctest `--output-on-failure` plus `-V` for that test. I did not change the
workflow; that was not in the brief.

## Tests (Linux clang-asan, HEAD 3d8a32406)
- vtextension_test: all passed, 487 cases / 26731 assertions.
- vtextension_process_test: 141 cases, 140 passed, 1 skipped.
- vtextension_helper_test: 161 cases, 160 passed, 1 skipped.
- vtextension_wamr_helper_test: all passed, 62 cases.
- vtextension_remote_test: all passed, 302 cases.
- vtextension_remote_wamr_test: 55 cases, 54 passed, 1 skipped.
- macOS channel paths forced on Linux: all three platform constants switched to their `__APPLE__` values, then
  reverted. Channel.* and PduStream.* all passed (30 cases). The POLLHUP check gives Closed there too: Linux raises
  POLLHUP once the peer shut down both ways.
- e135d799f was built on its own, and its Channel cases ran.
- clang-tidy 22 on Channel_posix.cpp and Channel_test.cpp (which pulls in ChannelStrings.hpp): no findings.
- clang-format 22 `--dry-run --Werror` and editorconfig-checker: clean.

## Concerns
1. **Not proven on macOS itself.** The fix rests on XNU source:
   - `uipc_send()` never checks the peer, and `poll()` maps EV_EOF to POLLHUP.
   - Running it on Linux proves only that the check does not misfire. Linux would have returned Closed anyway.
   - If macOS's poll does not raise POLLHUP after a peer's `shutdown()`, the two cases fail again, now printing
     `value …`, which would settle it.
2. **The once-per-send poll cost on macOS** has not been measured. The control and lane PDUs are small and few, but
   a large Load is sent in one `send()` loop, so it pays one poll per kernel write.
3. **The Unasserted probe values are still unknown** (see above). The workflow needs a step that prints them.

## Round 1 (review minors 1-3)

All three minors are folded into the fix commit, which is now cde2d3e42 (was 3d8a32406), with its message updated.
e135d799f is unchanged. Not pushed.

1. **The SHUT_RDWR invariant is documented.** `refuseSendToEndedPeer()` asks whether *this end's receiving side*
   ended. That means the peer's whole stream ended only because no end is ever shut one way alone:
   `Channel::shutdown()` is the only shutdown, and it uses SHUT_RDWR. The function's doc and a comment in
   `Channel::shutdown()` now say so.
2. **One poll per chunk in trySend on macOS.**
   - `sendableWithoutWaiting()` now returns `std::expected<std::size_t, ChannelError>`.
   - On macOS it makes one `poll(POLLOUT | POLLIN)` (POLLIN only where `SendToEndedPeer::Dropped`), retrying on
     EINTR. POLLHUP gives Closed; no room gives 0; an error pending or this end's own shutdown is left to the send.
     The comment is reworded to say exactly that.
   - Where MSG_DONTWAIT is honoured, it calls `refuseSendToEndedPeer()`, which does nothing outside macOS.
   - `trySend()` no longer calls `refuseSendToEndedPeer()` separately.
3. **sendWithHandle is tested.** New case `Channel.sendWithHandleToAShutDownPeerIsClosed`: after `helper.shutdown()`,
   `parent.sendWithHandle()` returns Closed.

Tests:
- Linux clang-asan vtextension_test: all passed, 488 cases / 26734 assertions.
- Forced-macOS-branches Linux build (all three platform constants switched, then reverted): Channel.* and PduStream.*
  all passed, 31 cases / 627 assertions.
- clang-tidy 22 on Channel_posix.cpp and Channel_test.cpp: no findings. clang-format 22 and editorconfig-checker:
  clean.

Review item 4 (squash into a14efb900 before merge) and item 5 (XNU's `soshutdownlock_final` may not wake a thread
blocked in `::send` on this end) are noted for later, as the review asks; neither is changed here.

### Round 1 correction
Channel.sendWithHandleToAShutDownPeerIsClosed moved out of the `#ifdef __linux__` block into the POSIX-wide section (makePipe and sendWithHandle are both available there), so it runs on macOS. Folded into the fix commit. Linux ASan Channel.*: 15 cases / 495 assertions passed; forced-macOS build Channel.*: 15 / 495 passed.
