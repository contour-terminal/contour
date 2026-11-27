# Run rules — every implementer and reviewer reads this

## Where and how
- Work ONLY in `/home/christianparpart/projects/contour-extension-host` (branch `feature/out-of-process-extension-host`). Never touch `/home/christianparpart/projects/contour` (the owner's main checkout) or other worktrees.
- Never push. Never edit `vendor/core-cpp`. Never `git add` anything under `docs/superpowers/` or `.superpowers/` (local, excluded).
- In every shell: `unset $(env | grep -o '^CONTOUR_[A-Z_]*')` before running tests or CLI verbs (inherited CONTOUR_* vars reach the owner's live daemon).

## Resource limits (another session is running on this machine — be gentle)
- One build at a time, one tree: `out/clang-asan` in this worktree.
- Configure: `PACE_SCHEDULER=127.0.0.1:6674 cmake --preset clang-asan` (fresh tree) or add `-DPACE_SCHEDULER=127.0.0.1:6674` when reconfiguring.
- Build: `flock -o /tmp/contour-extension-host.build.lock nice -n 19 ionice -c 3 cmake --build --preset clang-asan -j 4 --target <only the targets you need>`.
- Tests: run the focused test binary/cases; `ctest --preset=clang-asan -R vtextension` once before committing. No other presets unless the task says so.
- Never kill processes by name or pattern; only PIDs you started yourself.

## Code rules (AGENT.md is authoritative: /home/christianparpart/projects/contour-extension-host/AGENT.md)
- C++23, range-for only, `std::expected` with a per-subsystem error enum, `enum class : uint8_t` (zero = negative/absent) instead of bool,
  DI for every OS resource/clock/process, complete constructors, data-driven tables, `.hpp` + `#pragma once`, Doxygen (`///`) on public API,
  `[[nodiscard]]`, one declaration per line, Types CamelCase / everything else camelBack / `_member`.
- Zero warnings: -Werror, clang-tidy clean, no NOLINT, no #pragma mutes.
- No `| std::ranges::to<>()`, no `std::views::enumerate` (use `core::views::enumerate`). No `co_await` inside Catch2 assertions.
- Hoist `static_cast` out of Catch2 `CHECK()` for sign comparisons (clang-tidy modernize-use-integer-sign-comparison).
- Format changed C++ with `/usr/bin/clang-format -i` (never .qml). Run `editorconfig-checker` on changed files.
- User-facing text never says wasm/WAMR/Wasmtime; internal code may.

## Commits
- `git commit -s`, conventional subject (`feat(vtextension): …`, `build: …`), body explains why. NO Co-Authored-By / Claude-Session trailers.
- Never mention the plan, spec, task numbers or "phase" in commits or source.
- Build-system/packaging and code changes may share a commit only when inseparable.
