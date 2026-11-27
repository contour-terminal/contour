# Handover — out-of-process extension host (draft PR #2431)

Written 2026-11-27. **This commit is for handover only: drop it on the receiving PC** after copying its
files out (see "On the receiving PC"). Nothing here may land on the PR branch: `docs/superpowers` and
`.superpowers` are never committed.

## What this is

An SDD (subagent-driven) run of `docs/superpowers/plans/2026-11-24-out-of-process-extension-host.md`
(spec beside it in `docs/superpowers/specs/`). The goal is faster extensions than WAMR: AOT/JIT through
Wasmtime, inside `contour-extension-host`, one sandboxed helper process per extension. WAMR stays inside
the helper for Fedora RPM, offline Flatpak and FreeBSD. User-facing names never say wasm, WAMR or
Wasmtime.

- Branch: `feature/out-of-process-extension-host`, draft PR #2431 (labels: `no changelog`, `ci-freebsd`, `ci-arm`).
- Pushed head: `54387ba81` (the commit under this one). Master was unmoved at push time.
- Phases 1–4 are done. Phase 5 (sandboxes) is at task 5.8, its gate, in CI-fix rounds. Phase 6 (6.0–6.6: cache,
  packaging, docs, gates) has not started.

## Where it stands

- **CI on 54387ba81** (run 45855496567) was in progress at handover. The local CI watcher was stopped by
  memory pressure; check the run by hand:
  `gh run view 45855496567 --json jobs --jq '.jobs[]|"\(.name)\t\(.conclusion)"'`.
- **It must confirm:**
  - **Windows:** the handle-count fix (per-direction channel events created once; wine always reports
    0 handles, so this was never checked locally). The AppContainer probe now starts (DETACHED_PROCESS fixed
    0xC0000142). The container-folder and spawn (367) fixes are expected to pass.
  - **macOS:** `Channel.shutdownEndsThePeersStream` and `Channel.trySendNeverBlocks`. The fix: before each send
    on macOS, poll for POLLHUP, because XNU drops sends to a peer that shut down. This rests on XNU source
    reading only. If it fails, the cases now print the actual value (ChannelStrings.hpp StringMaker).
  - **FreeBSD and ARM** were green on the previous run.
- **Implementers** (agent ids belong to the old session and cannot be resumed): use fresh ones. Briefs and
  reports for every round are in `.superpowers/sdd/2026-11-24-out-of-process-extension-host/`
  (`task-5.8-ci5-*` is the latest).

## Next steps, in order

1. Read the CI result. Fetch the logs of any failed leg:
   `gh api --allow-escape-sequences repos/contour-terminal/contour/actions/jobs/<jid>/logs | sed 's/\x1b\[[0-9;]*[A-Za-z]//g'`.
   Then write the next brief, dispatch it, review it, and push.
2. Rule on the macOS Unasserted probe rows (AF_ROUTE, AF_SYSTEM, SIOCGIFCONF). CI does not show their
   outcome yet: a workflow step that runs the process suite verbosely on macOS would.
3. Step 9 phase summary, close 5.8, then phase 6 (6.0–6.6).
4. **Pre-merge:**
   - a per-commit build check (`git rebase --exec`);
   - squash the CI-fix commits into a few semantic commits (fold 3d8a32406's successor into a14efb900's);
   - docs: docs/extensions.md:343, D109 residual risks;
   - release note, then drop `no changelog`;
   - Windows signing list (owner);
   - PR risk notes: Wasmtime OOM detection relies on message text; priming allocation is untested; system
     WAMR is unpatched on Linux/FreeBSD (a harmless race); Windows-only tests are proven only on CI.
5. **Follow-ups (separate):**
   - report the WAMR cluster race and the MAP_JIT change upstream, then replace the Apple refusal with a
     version check;
   - an XNU blocked-send wakeup after peer shutdown (test when a Mac is at hand);
   - WAMR hostile twin;
   - rename `exitsCleanlyWithAnEmptyEnvironment`;
   - `readReport`;
   - `DefaultBackend` RemoteKindState;
   - duplicate `wouldBlock` in core-cpp;
   - FreeBSD port pkg-plist.

## Binding rules of the run

- Push only to `feature/out-of-process-extension-host`. Never mark #2431 ready.
- Rebase onto origin/master before every force-push, and push with `--force-with-lease`.
- Never edit `vendor/core-cpp`. Never commit `docs/superpowers` or `.superpowers`.
- `unset $(env | grep -o '^CONTOUR_[A-Z_]*')` in every shell. Kill processes only by recorded PID.
- Commits use `git commit -s` (Signed-off-by: Christian Parpart <christian@parpart.family>), with no AI
  trailers. Each commit builds on its own.
- Builds run niced:
  `flock -o /tmp/contour-extension-host.build.lock nice -n 19 ionice -c 3 cmake --build --preset clang-asan -j 4 --target …`.
  Run one subagent and one tree at a time while another heavy session is running.
- Use clang-format 22 at `/usr/bin/clang-format`.
- The Windows cross toolchain is /opt/msvc (msvc-wine) with clang-cl and wine.

## Files in this commit

- `docs/superpowers/plans/2026-11-24-out-of-process-extension-host.md`: the plan. This is the worktree copy, which is newer than
  the main checkout's.
- `docs/superpowers/specs/2026-11-24-out-of-process-extension-host-design.md`: the spec.
- `.superpowers/sdd/2026-11-24-out-of-process-extension-host/`: the ledger (`progress.md`, the recovery map:
  trust it and `git log` over memory), plus the briefs, reports, reviews and review diffs.
- `STATUS.md`: the status board source. Its page is https://claude.ai/artifact/9QY8hXTCuX9cqpo8KtkKRM.
- `.superpowers/handover/status-board.times.jsonl` and `status-board.url`: the board's time log and URL,
  which normally live in the worktree's git dir.

## On the receiving PC

```bash
git fetch <remote> handover/out-of-process-extension-host
git worktree add ../contour-extension-host feature/out-of-process-extension-host   # or reuse one
cd ../contour-extension-host
git checkout FETCH_HEAD -- docs/superpowers .superpowers STATUS.md   # copies the files out
git reset -q                                                          # unstage them; they stay local
cp .superpowers/handover/status-board.* "$(git rev-parse --git-dir)/"
grep -qx '/STATUS.md' "$(git rev-parse --git-path info/exclude)" || echo /STATUS.md >> "$(git rev-parse --git-path info/exclude)"
```

`.superpowers` and `docs/superpowers` must also be excluded on that machine (check `git check-ignore -v`).
Then drop the handover branch. Resume with superpowers:subagent-driven-development on the plan. The
ledger's `references/resuming.md` procedure picks up at 5.8.
