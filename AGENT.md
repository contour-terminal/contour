# C++ Coding Guidelines

- Prefer C++23: `constexpr`, `std::ranges`, `std::format`, `std::expected`, structured bindings
- C-style loops are forbidden; use range-based for loops exclusively
- Use `std::views::iota` and other views for generating and transforming ranges
- Use `std::span` for passing arrays and contiguous sequences
- Use `auto` type deduction to improve readability
- Use `const` correctness throughout (refs, pointers, member functions)
- Mark return values `[[nodiscard]]` where ignoring the result would be a bug
- Document new public functions, classes, structs, and their members using Doxygen style:
  ```cpp
  /// Short description of the function (be concise).
  /// @param name Description.
  /// @return Description.
  ```
- Naming conventions and static-analysis rules live in **per-module `.clang-tidy` files**
  (`src/.clang-tidy` is the base; `src/crispy/`, `src/text_shaper/`, and `examples/` override
  it). These files are the authoritative source and win over any prose here.
- C++ code formatting rules are defined in `.clang-format`; run `clang-format` after changes.
- Use smart pointers for ownership; do not use raw owning pointers
- Do not introduce new third-party dependencies without strong justification
- Do not suppress clang-tidy warnings with `NOLINT` comments; fix the underlying issue

## Design Patterns & Principles

Always aim for a clean software architecture. The following principles are load-bearing and
should be adhered to unless there is a very strong, explicitly justified reason not to.

### Error handling: `std::expected<T, E>`
Prefer `std::expected<T, E>` for fallible API surface. Give each subsystem its own error enum,
introduced *as the need arises* — do not invent a taxonomy up
front. Chain monadically with `and_then`, `or_else`, `transform`, `transform_error` rather
than nested `if`s. Reserve exceptions for programmer errors (precondition violation, contract
misuse), not for expected, recoverable failures.

### Dependency injection
**This is a load-bearing principle, not a nice-to-have.** Anything that
touches I/O, time, randomness, the filesystem, the network, or any other
ambient/global resource is reached through an interface — never through a
concrete type, a singleton, or a free function with hidden state.

### Configuration at construction time
**A constructed object is a usable object.** Everything a class needs to do its job —
collaborators, policy, tuning knobs, limits — is supplied to its constructor and is fixed
thereafter. No `init()`/`setup()` second phase, no default constructor followed by a run of
setters, no static knob poked from elsewhere at startup.

This is the **Complete Constructor** pattern, realized through **constructor injection** and
**immutability**; in C++ it is **RAII** generalized from resources to configuration. What it
forbids is **two-phase initialization** and the **temporal coupling** it creates — a hidden call
order the caller must know, and a not-yet-configured state every method must tolerate.

**Configuration is not state.** This governs how an object is *set up*, not what it does
afterwards. A setter that mutates the domain state the object exists to manage is fine:
`CellProxy::setForegroundColor()` is the cell's data; `InputGenerator::setCursorKeysMode()` is VT
state driven by DECCKM at runtime. `BoxDrawingRenderer::setBrailleStyle()` — a *static*
rendering-policy knob written once from `Config.cpp` — is the thing to avoid. Ask: *would two
differently-configured instances be two different objects, or one object in two states?*
Different objects → constructor.

- Omit the default constructor when there is nothing sensible to default to.
- Configuration members are private and have no setter. Prefer this encapsulated immutability
  over `const` members: a `const` member deletes copy- and move-assignment, quietly breaking
  types held in containers or reassigned. `const`/reference members are permitted
  (`cppcoreguidelines-avoid-const-or-ref-data-members` is off) — use them for value types that
  genuinely never need assignment.
- A long constructor is a fact about the *data*, not a reason to add setters: group related
  parameters into a config struct (which data-driven design wants anyway). A builder is for
  genuinely optional, order-independent parameters only.
- Never wire with a global. A `static` setter is post-construction configuration plus unbounded
  scope, no thread-safety, and state leaking between tests.
- Fallible setup belongs in a static factory returning `std::expected<T, E>` — not in a
  constructor that leaves the object half-built.

**When you cannot.** Each of these must be documented at the declaration, with the reason:

- **Live reconfiguration is the feature** — `Renderer::setFonts()` on config reload. Note the
  price `Renderer` pays for it: `_reconfigMutex`, staged-vs-published state,
  `applyPendingReconfig()`. Pay it only where a user-visible requirement demands it.
- **Externally-driven geometry** — `Renderer::setPageSize()`/`applyResize()`; the window manager
  decides, not us.
- **Framework-mandated** — QML default-constructs types and assigns `Q_PROPERTY`s.
- **Documented rebinding seams** — `TerminalDisplay::setSession()` exists so a session can move
  between displays; the seam is the design.
- **Cyclic wiring** — when A and B must know each other, one `attach`-style call after
  construction is acceptable; a *sequence* of them is not.

**Enforcement.** The mechanical half is automated: `cppcoreguidelines-pro-type-member-init` and
`cppcoreguidelines-prefer-member-initializer` are enabled, so every member is initialized, in the
member-initializer list. The design half is a review question — *how many calls must a caller
make before this object is usable?* The answer must be zero. This is also why it pays off for
testing: a fully-constructed object is built with test doubles in one expression, with no setup
ritual and no half-configured state to reason about.

### Data-driven design
**Behaviour is described by data; code interprets that data.** This is
equally load-bearing and goes well beyond "no magic numbers". The aim is
that adding a flag, a protocol verb, a storage backend, or an error code
is a matter of *adding a row to a table*, not editing logic scattered
across the codebase.

As with DI, **adhere to this unless there is a very strong, explicitly
justified reason not to.** When in doubt, ask: "if a sixth case showed up
tomorrow, how many places would I edit?" If the answer is more than one,
the design is not data-driven enough yet.

### Testability of every code area
**Every code area must be testable, and new code lands with tests.** Each module ships a
Catch2 `*_test` target (`vtbackend_test`, `vtparser_test`, `vtpty_test`, `vtworkspace_test`,
`crispy_test`, …). Code that is not headless-constructible (the GUI/RHI stack in
`src/contour/`) is made testable by *extracting pure decisions into dependency-free headers*
and driving the rest offscreen — the `contour_gui_test` / `vtworkspace_test` harnesses (GUI uses
`Qt6::Test`). If something is hard to test, that is a design smell: inject the dependency and
extract the decision, don't skip the test. Aim always to increase coverage.

## Zero-warning policy

**The codebase is warning-free, and a warning is a build break.** Dev and CI builds compile
with `-Werror`, enabled via `PEDANTIC_COMPILER_WERROR: ON` in `cmake/presets/common.json` and
inherited by every preset through `contour-common` (see `cmake/PedanticCompiler.cmake`).

- Fix the cause of a warning — never silence it. No `NOLINT`, no `#pragma` mutes, and no
  widening of `-Wno-error=…` without an explicit, justified reason.
- clang-tidy runs as part of the pedantic build; treat its diagnostics the same way — fix,
  don't suppress.

## Repository layout

First-party modules under `src/`, roughly bottom-up (later depends on earlier):

- `src/crispy` — foundational utilities (`result`, ranges/format helpers, app scaffolding)
- `src/text_shaper` — font shaping / glyph rasterization abstraction
- `src/vtparser` — VT escape-sequence parser (state machine)
- `src/vtpty` — pseudo-terminal (PTY) abstraction
- `src/vtbackend` — terminal engine (grid, screen, VT semantics)
- `src/vtrasterizer` — turns terminal cells into renderable geometry/atlases
- `src/vtworkspace` — Qt-free tab/split-pane tree model
- `src/contour` — the Qt/QML GUI frontend (windows, display, RHI renderer)

Respect these boundaries: lower layers must not depend on higher ones, and the GUI must not
reach around `vtbackend`/`vtworkspace` into rendering internals.

# VT reference sources

When implementing or reasoning about a VT/ANSI escape sequence's semantics, **cross-check against
both** the established terminal source trees **and** the primary DEC reference manuals. Primary
sources beat scattered web summaries; parity/divergence decisions must be grounded in what real
terminals actually do. Reading the source alone is not enough — verify against a manual too.

Point the environment variable `$CONTOUR_VT_REFERENCE_SOURCES` at a directory holding local
checkouts of the reference terminal source trees; the commands below assume it. The list of trees
and their public upstreams — plus how to wire the variable up per machine — lives in
[docs/internals/vt-conformance.md](docs/internals/vt-conformance.md#reference-sources). If the
variable is unset, clone the trees first (they are not vendored into this repo).

- **Terminal source trees.** `grep -rniE "<MNEMONIC>|<alt name>" "$CONTOUR_VT_REFERENCE_SOURCES/<tree>/"`
  to find the parser case, dispatch, and state. xterm's `ctlseqs.txt` is the canonical sequence
  catalog; its `charproc.c` / `VTparse.def` / `ptyx.h` hold the handlers and state. Windows Terminal:
  `src/terminal/adapter/adaptDispatch.cpp` + `OutputStateMachineEngine.hpp` (the VTID table). Note
  where terminals **diverge or punt** — that is often where the interesting decision is.
- **DEC STD 070 (Video Systems Reference Manual)** — <https://j4james.github.io/vtdocs/> is a clean,
  readable text transcription of the DEC STD 070 pages (the VT420-era standard). Use it as the
  first-stop authority for sequence semantics and detail tables; it is far easier to navigate than
  the scanned PDFs.
- **VT520/VT525 Programmer Information manual (EK-VT520-RM)** is ground truth for VT5xx sequences.
  The archive.org full-text truncates chapter 5 over the web, but the full PDF is fetchable and
  readable page-by-page (Read with `pages:`). Chapter 5 (ANSI Control Functions) lists functions
  alphabetically; it is the only authority for detail tables the source trees don't render — e.g.
  DECATC's `Ps1` is an *enumerated* combination list (0=Normal, 1=Bold, 2=Reverse, 3=Underline,
  4=Blink, 5=Bold reverse, …, 15=all four), **not** a `Bold=1|Reverse=2|Underline=4|Blink=8` bitmask;
  assuming the bitmask is a real bug.
- **xterm is the only reference that must be *measured*, not just read** (run it under Xvfb) — for
  behaviours like DECRQCRA where the manual and the source disagree with what xterm actually emits.

# Building

- Linux debug (default): `cmake --build --preset clang-asan` — Clang, Debug, with
  Address + UndefinedBehavior sanitizers. On Windows use `clang-debug`.
- Release / performance: `cmake --build --preset clang-release`.
- Coverage: `cmake --preset clang-coverage` then `cmake --build --preset clang-coverage`
  (Clang, Debug, `CONTOUR_COVERAGE=ON` → `-g --coverage`; builds into `out/clang-coverage/`).

# Testing

- Run the suite: `ctest --preset=clang-asan` (for a release tree: `ctest --test-dir out/clang-release`).
- Framework is **Catch2** for the libraries; the GUI tests use `Qt6::Test`.
- Run a single test: `<module>_test "TestName"` with an optional `-c "section name"`
  (e.g. `out/clang-asan/src/vtbackend/vtbackend_test "TestName" -c "section"`).
- Code coverage: configure/build with the `clang-coverage` preset (above), then run the suite
  with `ctest --preset=clang-coverage`. This exercises the unit tests plus the offscreen,
  `e2e`-labeled app runs — including the coverage-oriented config at
  `src/contour/test/e2e/coverage-config.yml.in` (substituted by CMake), which deliberately
  drives the Config-parsing and live-render paths of the not-headless-constructible GUI/RHI
  stack. Clang emits gcov instrumentation data, readable via `llvm-cov gcov`.

# Workflow

- When done with the code changes, run the `/simplify` command and avoid code duplication.
- Zero-warning policy is non-negotiable: the build must be clean under `-Werror` (see above).
- Ensure changes are covered by tests and run them (`ctest --preset=clang-asan`).
- For perf-sensitive changes, check for regressions with Callgrind:
  `valgrind --tool=callgrind` and analyze with `callgrind_annotate`.
- In change summaries, report: performance impact (if any), a risk assessment, and code
  coverage results.
