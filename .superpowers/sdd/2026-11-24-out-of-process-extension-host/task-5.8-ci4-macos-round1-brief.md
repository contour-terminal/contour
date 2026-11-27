# ci4-macos — fix round 1 (review of ce9062c11..ff4e3d5e1)

Fold into the five ci4-macos commits where it is clear which (fixup + autosquash within ce9062c11..HEAD is
safe: they are recent and self-contained), else a new commit; every commit builds; do not push.
Constraints as in task-5.8-ci4-macos-brief.md.

Important
1. SandboxProbe_test.cpp:256-275 (macOS table) — with socket() allowed, assert what the profile still
   stops, as new probe ops/rows: sendto on an unconnected UDP socket (Internet address), bind+listen on an
   Internet address, bind on a Unix path, socket(AF_ROUTE), socket(AF_SYSTEM) (where the SDK has it), and
   ioctl SIOCGIFCONF on the permitted AF_INET socket. Where the outcome is genuinely unknown, mark the row
   `Unasserted` so the next CI log reports it (the brief's diagnostic rule); where Seatbelt's documented or
   observed behaviour says refused, assert refused. Add the same ops to the Linux/FreeBSD tables with
   their expected outcomes (they are already locked down there — check against the seccomp/Capsicum
   policy; don't guess: Unasserted if unsure). Fix the "every address is refused" comment to say exactly
   what the rows prove.

Minor
2. SandboxProbe.cpp `permitted` op: getpagesize() may be cached before lockdown — call
   sysctlbyname("hw.pagesize_compat") (the exact rule) after lockdown instead, so the row guards the rule.
3. Channel.hpp:76-81: state "one sender per end at a time" in the header contract (trySend relies on it on
   macOS).
4. ContourThirdParties.cmake system-WAMR branch: on Apple (any arch where the hardened runtime applies) a
   system WAMR lacks the MAP_JIT patch — refuse it at configure time with a clear message (FATAL_ERROR),
   since such a build would fail every instantiation.
5. Channel.hpp:716-719: fix the broken reflowed doc sentence.
6. Channel_posix.cpp receiveSome macOS branch: on MSG_CTRUNC log once (warning category used elsewhere in
   this module) that control data was truncated, and do not ignore Ancillary::Foreign silently — close and
   log consistently with the unasked-descriptor path.

Run Linux ASan for touched targets (vtextension_test, vtextension_process_test, helper suites as affected),
clang-tidy/clang-format 22. Append a round-1 section to task-5.8-ci4-macos-report.md; return head, one-line
tests, concerns.
