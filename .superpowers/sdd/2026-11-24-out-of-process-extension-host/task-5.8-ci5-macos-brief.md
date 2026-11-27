# Task 5.8 — CI round 5, macOS

Worktree /home/christianparpart/projects/contour-extension-host, branch feature/out-of-process-extension-host.
CI run 45826489690 ran on a954a9112. Local HEAD includes c66b53a20 (your round-3 minors, unpushed) and possibly Windows round-5 commits.
Full macOS log: /tmp/claude-1000/-home-christianparpart-projects-contour/a3633faa-27fc-44d2-bb77-a2354e68aa02/scratchpad/ci/macOSArm-a954a.log (vtextension_test failures near lines 8460-8610).

## Progress
The sandbox probe cases now pass on macOS, including the lockdown, and so do the Wasmtime helper and the unentitled-kill checks. Only two Channel cases fail.
Also report, from the log, what each Unasserted probe row (AF_ROUTE, AF_SYSTEM, SIOCGIFCONF) actually returned. If the log does not show it, say so.

## Failures to fix (find the root cause first; use superpowers:systematic-debugging)
1. **Channel.shutdownEndsThePeersStream** (Channel_test.cpp:126): `helper.send(1024 bytes)` after the peer shut down did not
   return ChannelError::Closed on macOS.
2. **Channel.trySendNeverBlocks** (Channel_test.cpp:270): `parent.trySend(chunk)` after the peer is gone did not return Closed.
   You flagged this concern earlier, because it had never run on macOS.
Catch2 printed both sides as {?}. Find what Channel_posix returns there on macOS (for example, EPIPE vs. ECONNRESET, a socket
without SO_NOSIGPIPE, the poll-before-send path seeing POLLHUP or POLLOUT differently, or a send that succeeds into the buffer after
shutdown(SHUT_RD) on the peer). Fix the classification in Channel_posix so the contract holds on every POSIX platform, not by
special-casing the test. Also give ChannelError (and the expected type if needed) a Catch2 StringMaker, so a failure prints
its value in future.

## Constraints (binding)
Same as before: unset CONTOUR_* in each shell; flock -o /tmp/contour-extension-host.build.lock nice -n 19 ionice -c 3, -j 4; PIDs only;
never edit vendor/core-cpp; never commit .superpowers or docs/superpowers; git commit -s with Signed-off-by: Christian Parpart
<christian@parpart.family>, no AI trailers; fold fixes into the introducing commit (fixup + autosquash), each commit builds alone;
do NOT push; no subagents; no wasm, WAMR or Wasmtime in user-facing text; clang-format 22 at /usr/bin/clang-format; AGENT.md rules.

## Report
Write the full report to /home/christianparpart/projects/contour-extension-host/.superpowers/sdd/2026-11-24-out-of-process-extension-host/task-5.8-ci5-macos-report.md. Return only status, commits, a one-line test summary and concerns.
