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
- **Header files use the `.hpp` extension.** `.h` is reserved for what is genuinely a C header —
  system and third-party headers, and files shared with GLSL (`src/vtrasterizer/shared_defines.h`
  is the sole first-party example). Every header carries `#pragma once`, never an include guard.
- Naming conventions and static-analysis rules live in **`.clang-tidy` files** (`./.clang-tidy` at
  the repository root is the base, so every first-party tree resolves to it by clang-tidy's normal
  parent-directory search; `src/crispy/` and `src/text_shaper/` override it for their snake_case
  naming). These files are the authoritative source and win over any prose here.
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

### `enum class` over `bool`
**A `bool` in an API is an anonymous enum whose two values are named after their representation
instead of their meaning.** A `bool` parameter, return type, or data member is a finding unless one
of the exceptions below applies; the replacement is a purpose-named `enum class`. The tree already
has dozens of these and they are the model to copy: `WrapPending`, `HighlightSearchMatches`,
`JumpOver`, `StatusLineStyling`, `PixelReporting`, `ClusterWidthPolicy`, `GlyphWidthPolicy`.

The parameter case carries all three costs at once:

- **The call site loses the meaning.** `Line::toUtf8Trimmed(true, false)` tells a reader nothing,
  and no amount of careful naming *inside* the function repairs the code that calls it.
- **The compiler stops helping.** `bool` accepts pointers, integers and characters through implicit
  conversion, so an overload taking `bool` can quietly swallow an argument meant for another one,
  and two adjacent `bool` parameters can be exchanged without a diagnostic. An `enum class`
  converts from nothing.
- **A third case rewrites every signature.** When yes/no becomes yes/no/inherit, a `bool` forces a
  signature change and an edit at every call site; an `enum class` gains an enumerator and `switch`
  exhaustiveness names the places that must now handle it — the same argument data-driven design
  makes.

`TerminalSession`'s guarded-role API is the case to learn from: `applyPendingFontChange`,
`applyPendingPaste`, `executePendingBufferCapture` and `executeShowHostWritableStatusLine` all take
`(bool allow, bool remember)`, as does `executeRole(GuardedRole, bool, bool)`. The two arguments
are adjacent, identically typed and silently exchangeable, and together they gate a *permission*
decision — swap them at one call site and you grant what should have been remembered. Also
`Terminal::programUDK(bool clearAll, bool locked, …)`, `tabVisualStateFor(bool active,
bool hovered, bool windowActive)`, and `Terminal::refreshRenderBuffer(bool locked = false)`, which
manages to be both a defaulted `bool` parameter and a `bool` return.

**The shape of the fix.**

- **Name the enum after the decision, not after the type.** Domain words beat `Yes`/`No`
  (`ClusterWidthPolicy::FirstCodepoint`); reserve `Yes`/`No` for a type whose own name already
  reads as the question, as `JumpOver::Yes` and `HighlightSearchMatches::No` do.
- **Give it an explicit underlying type** — `: uint8_t`, as nearly every enum in `src/` already
  does. Order the enumerators so the off/absent/default case is zero. We are inconsistent here
  (`WrapPending` is `{Yes, No}`, `HighlightSearchMatches` is `{No, Yes}`); new enums use zero for
  the negative case so a zero-initialized value still means what the `false` meant.
- **Flags that genuinely combine are a bitmask, not a pile of enums.** Where several booleans are
  truly orthogonal and every combination is legal, the answer is one bitmask type — this is why
  `cppcoreguidelines-use-enum-class` is off for `vtbackend::Modifier`, `LockKey` and
  `MatchModes::Flag`, whose values are protocol-defined bit positions.
- **A strong typedef is the other acceptable shape** where a value must stay boolean in behaviour
  but distinct in type — `using Handled = boxed::boxed<bool, HandledTag>;` on `InputHandler`. Reach
  for it when the type name supplies the meaning and the two states have no better names.

**Per position.**

- **Parameters.** A defaulted `bool` is the worst form — it shows neither the name nor the value at
  the call site, so prefer two named functions. Two adjacent `bool` parameters are the next worst,
  being silently exchangeable; fix those signatures first.
- **Returns.** A `bool` return is right when the function name *asks the question*: `empty()`,
  `contains()`, `isLineWrapped()`, `hasLineAt()`, the comparison operators. It is a finding when it
  reports success or failure — `captureScreen()`, `loadConfig()`, `tryAddKey()`, `openUrl()`,
  `runDetached()` are all really `std::expected<void, E>`, which carries the *reason* instead of
  discarding it — or when it selects between two named outcomes, which is an `enum class`.
- **Members.** The same test as parameters, plus one more: two or more `bool` members in a type are
  usually a state machine hiding in flags. Where some combinations cannot legally occur, the states
  are one `enum class`, not a set of independent switches.
- **A surviving `bool` reads as a predicate** — `_isVisible`, not `_visible` — so the use site
  still reads as a question.

**When you cannot.** Each of these must be documented at the declaration, with the reason:

- **The parameter is the property** — a setter that exists only to assign a `bool` member which
  itself passed the test above. The narrow carve-out, not a general licence: if the member should
  have been an `enum class`, so should the setter.
- **A signature you do not own** — a Qt virtual or slot, a standard concept (`std::predicate`,
  comparators), a C callback typedef.
- **Serialization and wire boundaries** — `Config` fields parsed from YAML, protocol flags. Convert
  at the boundary and keep the `enum class` inside it. `Config`'s public `bool` fields are the
  documented exception; do not open a sweep of the config schema on their account.
- **Generic code with no domain meaning** — a `bool` template argument threaded to `if constexpr`,
  as in `Parser.hpp`'s `TraceStateChanges`.
- **QML-facing `Q_INVOKABLE`/`Q_PROPERTY`** — an enum reaches QML only once registered with the
  meta-object system (`Q_ENUM`), and the QML side must then name the enumerators. Worth doing, but
  find the QML callers before promising it.

**Enforcement.** Mostly a review question — *at the call site, can you tell what `true` means
without opening the header?* — because the two checks that would help are off:
`bugprone-easily-swappable-parameters` (adjacent parameters of convertible type) and
`readability-implicit-bool-conversion` (the conversions that let a `bool` overload swallow a
pointer). A clean build therefore says nothing about signatures; enabling either on one module is a
reasonable first move, and what it reports is the finding, not noise. Where a `bool` is
deliberately kept, `bugprone-argument-comment` is enabled via `bugprone-*`, and its
`CommentBoolLiterals` option (off by default) would make `/*asciiHint=*/true` a *checked* comment
rather than a hopeful one — a mitigation, not a substitute for the type. The practice barely exists
today: 27 such comments in `src/`, all but one of them in tests, against ~165 bare literal
arguments in non-test code. Tellingly, `/*allow=*/` and `/*remember=*/` are among the 27 — the
guarded-role call sites already felt the need to annotate what the type should have said.

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
- `src/contour` — the application: the headless CLI, and the Qt/QML GUI frontend

Respect these boundaries: lower layers must not depend on higher ones, and the GUI must not
reach around `vtbackend`/`vtworkspace` into rendering internals.

`src/contour/` is itself layered, one directory per concern, and **each directory names the
namespace it holds** — `src/contour/config/` is `contour::config`, and so on for every one. Only
`main.cpp` and `ContourGuiApp` (the composition root) sit at the top. Lowest first:

- `config/` — the configuration model, its parsing and its persistence. **Qt-free**, and the
  `contour_config` target links no Qt at all. `Actions` lives here too, in `contour::actions`:
  it is the vocabulary the key bindings bind to.
- `command/` — the command palette's vocabulary and the context menus built from it. **Qt-free**;
  what a menu row *is* is decided here and rendered a layer up.
- `cli/` — the headless application: every `contour <verb>` that opens no window. **Qt-free**, and
  what a `CONTOUR_FRONTEND_GUI=OFF` build consists of.
- `geometry/` — page ⇄ pixel geometry, spoken by session, display and window alike.
- `input/` — Qt event → `vtbackend` input translation, with **no knowledge of `TerminalSession`**:
  what a chord *means* is decided here, where it is *delivered* a layer up.
- `platform/` — adapters to Qt and the OS: bell, blur-behind, notifications, speech, URL opening.
  Each splits into the decision and the adapter (`NotificationRouter` vs `FreeDesktopNotifier`),
  so the decision stays testable when the resource is absent.
- `session/` — one terminal session, the session-lifetime service, and the input routed into them.
- `display/` — the `QQuickItem` terminal, the RHI renderer and the accessibility interfaces.
- `window/` — the per-OS-window Qt/QML adapters (window controller, QML-facing models).
- `remote/` — the daemon and tmux attach engines.
- `qml/`, `styles/`, `res/`, `packaging/` — the QML modules, resources and installer inputs.

The Qt-free half is not a convention but a build fact: `contour_config`, `contour_command` and
`contour_cli` are separate library targets that do not link Qt, so reaching for a `QObject` from
the configuration model is a build error rather than a review finding.

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
- Data races: `cmake --preset clang-tsan` then `cmake --build --preset clang-tsan` — Clang, Debug,
  `CONTOUR_SANITIZE=thread`. ASan and TSan are mutually exclusive, hence the separate tree
  (`out/clang-tsan/`). Use it for anything touching the GUI-thread/parser-thread boundary.
- Coverage: `cmake --preset clang-coverage` then `cmake --build --preset clang-coverage`
  (Clang, Debug, `CONTOUR_COVERAGE=ON` → `-g --coverage`; builds into `out/clang-coverage/`).

# Testing

- Run the suite: `ctest --preset=clang-asan` (for a release tree: `ctest --test-dir out/clang-release`).
- Framework is **Catch2** for the libraries; the GUI tests use `Qt6::Test`.
- Run a single test: `<module>_test "TestName"` with an optional `-c "section name"`
  (e.g. `out/clang-asan/src/vtbackend/vtbackend_test "TestName" -c "section"`).
- Data races: `ctest --preset=clang-tsan`. Tests that drive terminal state from a second thread
  carry the `[threading]` tag, so `<module>_test "[threading]"` runs just those.
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
