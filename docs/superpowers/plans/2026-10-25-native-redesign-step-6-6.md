# Native Redesign — Step 6.6 (Secure Input: a Lock While the TTY Hides Typing, Secure Keyboard Entry on macOS) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

Written ahead (pipelining ruling, 2026-10-25). Line numbers are at HEAD `661b6029`. Every earlier step runs first:
4a is done, 1c is in progress, and 1d, 2, 3a–3f, 4b, 5a and 5b are written. Where this step uses code those steps
add, the task names it as their plans do and carries a **Depends on Step <id>** note. A pre-flight scan re-verifies
every reference before Task 1.

**Goal:** While the program in a pane has the terminal hide what is typed (echo off in canonical mode: `sudo`, `ssh`,
`read -s`), that pane shows a lock in its pane header or on its tab, and on macOS Secure Keyboard Entry is on for as
long as that pane holds focus in the active app, and is always turned off again.

**Architecture:**
- **vtpty says what the line discipline does with typed text.** `vtpty::Pty` gains
  `inputEcho() → InputEcho { Unknown = 0, Echoed, Raw, Hidden }` with an `Unknown` default; `UnixPty` reads the
  master's termios (`tcgetattr`), and a pure `inputEchoOf(tcflag_t)` decides: `Hidden` only for `!ECHO && ICANON`, so
  a full-screen program in raw mode (`vim`, `htop`, zsh's line editor) is `Raw`, never `Hidden`.
- **The decisions are pure.** `window/SecureInputPolicy.hpp` decides, from the configured mode and a session's echo,
  whether the pane is locked; from the app's activity and the focused session's echo, whether the OS should be in
  secure input; and from whether the pane header and the tab well are shown, where the lock is drawn.
- **The OS is an interface, and calls stay balanced by construction.** `platform::ISecureInput` (null inline, a Cocoa
  `.mm` calling `EnableSecureEventInput`/`DisableSecureEventInput`) is only ever called by
  `platform::SecureInputGate`, which follows a desired state and calls `enable()` on Off→On and `disable()` on On→Off
  only; its destructor turns an On state off. The gate lives in `TerminalSessionManager`, which re-evaluates it on
  every focus, echo, window-activation and teardown change.
- **QML renders the answer.** The tab, the pane header (Step 2) and, when neither is shown, a title-band chip draw the
  lock from tokens and a bundled `lock-symbolic.svg` (Step 5b's icon convention); the Terminal row draws `[secure]`
  cells.

**Tech Stack:** C++23, POSIX termios, Qt 6.11 (Quick, Quick Controls), QML, Objective-C++ with ARC (Carbon's
HIToolbox secure event input), Catch2 v3, CMake build tree `out/appleclang-debug`.

**Spec:** `docs/superpowers/specs/2026-10-24-native-redesign-implementation-design.md` §3.16 row 6.6 binds, with what
it cites: §1 (principles; 3 pure decisions, 4 the OS through interfaces, 5 nothing over non-blank cells, 7 AGENT.md),
§2.1 (token rows), §2.5–§2.6 (snapshots and testing), §2.7 (`DefaultsEdition`). The design record is
`docs/superpowers/specs/2026-10-23-native-redesign-design.md`. The canvas is
<https://claude.ai/artifact/12ASSTRD5ua5SwarsMJXwm>; its local copies `moments` (9 · Passwords and input methods),
`kit-components` and `tui` are in `.superpowers/design/canvas/project/`. The plans index is
`docs/superpowers/plans/2026-10-24-native-redesign-plans-index.md`.

## Status board and handoff (every step)

This run is tracked on a status board: <https://claude.ai/artifact/YZUVcC1vdaWaLGJLsdS2AX>. The
owner asked for the link to live here so that any agent continuing the work can find the board and
keep it current. The page is kept with `/status-board` (the `contour-workflows` plugin) on the machine doing the work; its source is local to that working tree and never committed.
- **Keep it up to date** with `/status-board update`, which republishes to the link above
  (`/status-board` remembers the link per working tree). The board belongs to the owner's account, so a
  session signed in as the owner can read and publish it; the design canvas, the owner's copy of
  Yaraslau's, is in the same account.
- **Every agent that continues this work updates the board.** Do it
  in the same turn as each state change: a step or task started, in review, committed, landed,
  blocked or unblocked, a plan written, or a ruling.
- A different worktree has a board of its own. Never write to another worktree's board.
  Report to the session that owns the board instead.
- If this worktree has no board, ask the owner rather than creating a second board.

## Global Constraints

- **The spec, verbatim (§3.16, row 6.6):** "Secure input | When echo is off, a lock badge, plus
  `EnableSecureEventInput` on macOS | `ISecureInput`". The brief adds: null elsewhere; "must always be turned off
  again (balanced calls; crash/close paths)".
- **Canvas values, exact** (`moments` 9): "When the tty stops echoing, a lock.fill symbol appears in the pane header /
  tab and macOS Secure Keyboard Entry turns on until echo returns." The pane header reads `sudo · Secure input on`: an
  11-pt lock glyph at `label.secondary` (62 %), gap 6, then the process name, ` · ` (U+00B7 between spaces) and the
  state text, in the header's footnote type. The lock's outline is the board's SVG, copied into the bundled icon:
  `<rect x="2" y="5" width="8" height="6" rx="1.5"/>` plus `<path d="M4 5V3.5a2 2 0 0 1 4 0V5" stroke-width="1.3"/>`
  on a 12 × 12 view box.
  - New token `size.secureInputGlyph`: 11 MacOS, 11 Fluent, 11 ContourLinux, 0 Terminal (cells draw text instead).
  - New `UiStyleTokens::secureInputText`: `""` on the three native rows (they draw the icon), `"secure"` on the
    Terminal row, wrapped in the row's `chipOpenGlyph`/`chipCloseGlyph` (`[secure]`).
  - Texts: `"Secure input on"` where `ISecureInput::support()` is `Native`, `"Input hidden"` elsewhere (owner
    decision 2). Accessible name of every lock: the same text.
- **Binding rulings** (ledgers 0, 1a, 1b, 1e, 4a; the SDD ledgers of 1c, 1d, 2, 3a–3f, 4b, 5b):
  - Every OS service is an interface in `contour/platform/` with a null implementation defined inline in its header
    and a test double, built through a `platform::AppearanceServices` factory that `nullAppearanceServices()` and
    `appearanceServicesWith(…)` (`test/AppearanceFixtures.hpp`) both name, so no `TestApp` reaches the native one.
  - AppKit: the `.mm` is ARC, inside the `if(APPLE)` block of `src/contour/platform/CMakeLists.txt` (`:84-103`) and in
    its `-fobjc-arc` list; every call is documented `@pre` main thread and asserted with `NSThread.isMainThread`; the
    non-Apple factory is in a `.cpp` wholly inside `#ifndef __APPLE__`; new headers are listed in
    `add_library(contour_platform …)` (`:15-77`).
  - Windows- and Linux-only code is verified only by CI, with a **Verified by** line. This step adds none: the
    Windows ConPTY and the null secure input compile everywhere and are exercised by the CI Windows job's `Test` step.
  - `[display]` cases skip offscreen. TestApp is Classic (R14) and passes `nullAppearanceServices()`.
  - No colour or size literal in chrome QML (`check-qml-colors`, `check-qml-sizes`; a new file's baseline is 0).
  - Every visual surface is a snapshot row, and every visual task renders the Terminal row
    (`CONTOUR_TEST_CONTROLS_STYLE=ContourTui`).
  - `enum class` over `bool`, zero the negative/default case; carve-outs are QML-facing `Q_PROPERTY`/`Q_INVOKABLE`
    booleans and `Config`'s public YAML `bool`s, each documented at the declaration.
  - Any new default goes through `DefaultsEdition` (§2.7): `secure_input` is an `EditionDefaults` column.
  - The Step 1b menu-owned macOS chords (⌘Q ⌘, ⌘H ⌥⌘H ⌘M) stay the menu's; nothing here binds a key.
  - Owner manual checks are batched per phase: listed in the ledger, never awaited.
- **AGENT.md applies in full:** dependency injection (the OS call only through `ISecureInput`), configuration at
  construction (`SecureInputGate` takes its `ISecureInput&` in its constructor; the manager takes the gate's
  collaborator from the app), data-driven tables (`SecureIndicatorRules`, the `SecureInputMode` spellings),
  `std::expected` where a call can fail, Doxygen (`///`, `@param`, `@return`, `@pre`) on every new public
  declaration, `.hpp` with `#pragma once`, zero warnings, no `NOLINT`; `slots`, `signals` and `emit` are never
  identifiers.
- **Build** with every core, configured `-DCONTOUR_EXTENSIONS_BUNDLED=OFF` (already in
  `out/appleclang-debug/CMakeCache.txt`): `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target <target>`.
  Never run two builds in one tree at once.
- **Tests:** each task runs only its own filter; the full suite runs in CI on Linux, macOS and Windows.
- **Lint:** before each commit run
  `/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) <the task's .cpp files>`
  and fix every finding; format every touched `.cpp`/`.hpp`/`.mm` with `/opt/homebrew/opt/llvm@22/bin/clang-format -i`;
  check every new or touched `.qml`, `.svg`, `.qrc`, `.txt` and `CMakeLists.txt` with `editorconfig-checker <files>`.
  **Never clang-format a `.qml` file.**
- **CI's clang 22 enforces `-Wmissing-designated-field-initializers`:** every new struct member gets `{}` or a value,
  and every table row names every field (`EditionDefaultsTable`, `ChromeTokenRows`, `SnapshotScripts`,
  `SecureIndicatorRules`, both null `AppearanceServices`).
- **Header-only functions are `inline` or `constexpr`.** `std::views::iota` takes two bounds of one type.
- **Qt-free stays Qt-free:** `config/SecureInputMode.hpp`, `window/SecureInputPolicy.hpp` and
  `platform/SecureInputGate.hpp` include no Qt.
- **Never edit `vendor/core-cpp`.**
- **Commits:** one per task. Stage only that task's files, by name (other agents share the tree; never `git add -A`
  or a directory). Each message ends with exactly `Signed-off-by: Christian Parpart <christian@parpart.family>`,
  never `Co-Authored-By`.
- After the code changes, run `/simplify` (Task 6).

## Review Focus

1. **A full-screen program turns echo off.** `vim`, `htop`, `less` and zsh's or bash's line editor all clear `ECHO`,
   with `ICANON` off too. Expected: no lock and no secure input at a shell prompt or in an editor; only a canonical-mode
   prompt (`sudo`, `ssh`, `read -s`) is `Hidden`. Pinned in Task 1 (`inputEchoOf` table and the real-PTY case) and
   Task 4 ("a raw-mode program shows no lock").
2. **Secure input left on after the prompt is gone.** The pane closes, its tab or window closes, the app quits, or
   another app comes to the front while the prompt waits. Expected: secure input is off after each, and the system's
   secure-input owner is no longer Contour. Pinned in Task 2 (the gate's destructor; the Cocoa round trip) and Task 4
   (close, window deactivation and manager teardown each end with one `disable()`).
3. **Unbalanced calls between two prompts.** Two windows each at a `sudo` prompt; focus moves between them, and one
   finishes while the other waits. Expected: `enable()` once and no `disable()` while focus moves between Hidden panes;
   one `disable()` when focus reaches an echoing pane. Pinned in Task 2 (`apply(On)` twice is one call) and Task 4.
4. **A background tab waits for a password.** `ssh` in tab 2 asks for a passphrase while the user types in tab 1.
   Expected: tab 2 shows the lock; secure input stays off until tab 2 is focused, then turns on. Pinned in Task 4.
5. **Panes whose echo cannot be known.** A daemon- or tmux-mirrored pane, an SSH-channel pane, a Windows ConPTY pane.
   Expected: `Unknown`, so never a lock and never secure input. Pinned in Task 1 (the `Pty` default) and Task 4 ("a
   remote-hosted session is never locked").

Also pinned, beyond the five: echo restored without output is noticed on the next key (Task 4); `secure_input: off`
shows no lock and never calls the OS (Tasks 3 and 4); the lock never covers cells (it lives in the header, tab or band).

---

## Spec resolutions (read before Task 1)

Those marked **(owner)** are repeated under *Owner decisions*; the run proceeds on their default.

- **R-a. What "echo is off" means.** `!ECHO && ICANON`. Raw mode clears both and is how every full-screen program and
  every line editor runs, so `!ECHO` alone would lock every shell prompt (Review Focus 1). Ghostty's password-input
  detection draws the same line; cross-check it against `$CONTOUR_VT_REFERENCE_SOURCES` at pre-flight if the trees are
  present (the variable was unset when this plan was written).
- **R-b. When the echo is read.** On the GUI thread, from the master descriptor, at three moments: the coalesced
  output notice of Step 1c (`TerminalSession::noteOutputForTab()`'s posted call; **Depends on Step 1c**), after every
  key or character the session writes to the PTY, and when the session gains focus. `sudo` and `readpassphrase(3)`
  turn echo off before they print the prompt, so the output notice sees it before the first key. No timer.
- **R-c. Where secure input applies.** On iff the configured mode is `Auto`, a window of this app is focused
  (`TerminalSessionManager::focusedWindow()` is set; it is cleared when another application comes to the front), and
  the focused session's echo is `Hidden`. Secure Keyboard Entry is process-wide, so one gate, in the manager.
- **R-d. Crash paths.** macOS ends a process's secure input when the process exits, so a crash cannot leave it on;
  the plan's duty is every in-process path (Review Focus 2), and the Finish step verifies the exit case by hand with
  `ioreg -l -w 0 | grep SecureInput`.
- **R-e. Lifetime.** `ContourGuiApp::_secureInput` is declared before `_sessionManager` (`ContourGuiApp.hpp:894`),
  next to `_taskbarProgressSink` (`:876`), so the gate in the manager is destroyed, and turns secure input off,
  before its `ISecureInput` is.
- **R-f. Where the lock is drawn.** On the tab whenever the tab well is shown and any of the tab's panes is locked;
  in a pane's header whenever that header is shown (Step 2's `resolvePaneHeader` answer); in a title-band chip when
  neither is shown and the active tab's focused pane is locked (one tab under `tab_bar: multiple`, one pane, no
  header). That is table `SecureIndicatorRules`.
- **R-g. The edition (owner, 1).** `secure_input: auto | off`, `Off` in Classic and `Auto` in Native2026: §2.7 keeps
  existing users' behaviour, and secure input blocks other apps' global hotkeys while it is on.
- **R-h. Remote panes.** The daemon protocol carries no echo state; a mirrored pane is `Unknown` (Review Focus 5). A
  protocol field is a follow-up, not this step.

## File structure

| File | Responsibility | Task |
|---|---|---|
| `src/vtpty/Pty.hpp`, `UnixPty.{hpp,cpp}`, `UnixUtils.hpp`, `Process.hpp`, `MockPty.hpp` (modify) | `InputEcho`, the termios read, forwarding, the mock's state | 1 |
| `src/contour/platform/SecureInput.{hpp,cpp}`, `SecureInputGate.hpp`, `CocoaSecureInput.mm` (create) | The interface, null, factories, the balanced gate, macOS | 2 |
| `src/contour/config/SecureInputMode.hpp` (create); `Config.hpp`, `Config.cpp`, `ConfigDocumentation.hpp` (modify) | `secure_input` | 3 |
| `src/contour/window/SecureInputPolicy.hpp` (create); `session/TerminalSession.*`, `session/TerminalSessionManager.*`, `window/WindowController.*`, `window/UiStyleProvider.*`, `ContourGuiApp.*` (modify) | Decisions and wiring | 4 |
| `src/contour/qml/SecureInputLock.qml` (create); `TabItem.qml`, `PaneHeader.qml`, `TitleBar.qml` (modify); `res/icons/lock-symbolic.svg` | The lock | 5 |

---

### Task 1: vtpty reports what the terminal does with typed text

**Files:**
- Modify: `src/vtpty/Pty.hpp:91-162` — the enum before `class Pty`, and the virtual with its default.
- Modify: `src/vtpty/UnixUtils.hpp` — `inputEchoOf` beside `getTerminalSettings` (`:75-80`).
- Modify: `src/vtpty/UnixPty.hpp:31-95`, `UnixPty.cpp` — the override.
- Modify: `src/vtpty/Process.hpp:172-185` — forward to `pty()`, in the "Pty overrides" block.
- Modify: `src/vtpty/MockPty.hpp:24-60` — `setInputEcho`, runtime state.
- Test: `src/vtpty/UnixPty_test.cpp`, `src/vtpty/UnixUtils_test.cpp`.

**Interfaces:**
- Produces (namespace `vtpty`, `Pty.hpp`):
  - `enum class InputEcho : std::uint8_t { Unknown = 0, Echoed, Raw, Hidden };` — `Echoed`: the terminal echoes;
    `Raw`: neither echo nor line editing (the program draws its own echo); `Hidden`: line editing without echo (a
    password prompt).
  - `class Pty` gains `[[nodiscard]] virtual InputEcho inputEcho() const noexcept { return InputEcho::Unknown; }`,
    documented "GUI-thread safe; a device that cannot tell answers Unknown".
- Produces (`UnixUtils.hpp`): `[[nodiscard]] constexpr InputEcho inputEchoOf(tcflag_t localFlags) noexcept;` —
  `ECHO` set → `Echoed`; else `ICANON` set → `Hidden`; else `Raw`.
- Produces: `UnixPty::inputEcho()` returns `Unknown` before `started()` or when `tcgetattr` on the master fails, else
  `inputEchoOf(tio.c_lflag)`. `Process::inputEcho()` returns `pty().inputEcho()`. `MockPty` gains
  `void setInputEcho(InputEcho echo) noexcept` (runtime state a test flips, documented as `setWriteBehavior` is) and
  answers it.

- [ ] **Step 1: Write the failing tests.**
  - `UnixUtils_test.cpp`, `[vtpty][inputecho]`, "inputEchoOf.decidesFromEchoAndCanonical": `ECHO | ICANON` →
    `Echoed`; `ECHO` → `Echoed`; `ICANON` → `Hidden`; `0` → `Raw`; `ICANON | ISIG` → `Hidden`.
  - `UnixPty_test.cpp`, `[vtpty][unixpty][inputecho]`, "UnixPty.inputEcho.followsTheSlavesTermios": open a
    `UnixPty`, start it, then on the slave descriptor (`pty.slave()`'s handle) `tcsetattr` three times and read the
    master after each: default (`Echoed`), `c_lflag &= ~ECHO` (`Hidden`), `c_lflag &= ~ICANON` (`Raw`).
  - "Pty.inputEcho.isUnknownUnlessTheDeviceCanTell": a `MockPty` answers `Unknown` until `setInputEcho(Hidden)`.
- [ ] **Step 2: Run them and see them fail.** Run:
  `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target vtpty_test`. Expected: compile error, no
  `vtpty::InputEcho`.
- [ ] **Step 3: Implement** the enum, the virtual, `inputEchoOf`, the three overrides and `setInputEcho`.
- [ ] **Step 4: Run them and see them pass.** Run: `out/appleclang-debug/src/vtpty/vtpty_test "[inputecho]"`.
  Expected: PASS (the real-PTY case proves `tcgetattr` on the master reflects the slave on macOS; CI's Linux jobs
  prove it there).
- [ ] **Step 5: Lint, format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) src/vtpty/UnixPty.cpp src/vtpty/UnixPty_test.cpp src/vtpty/UnixUtils_test.cpp
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/vtpty/Pty.hpp src/vtpty/UnixUtils.hpp src/vtpty/UnixPty.hpp src/vtpty/UnixPty.cpp src/vtpty/Process.hpp src/vtpty/MockPty.hpp src/vtpty/UnixPty_test.cpp src/vtpty/UnixUtils_test.cpp
git add src/vtpty/Pty.hpp src/vtpty/UnixUtils.hpp src/vtpty/UnixPty.hpp src/vtpty/UnixPty.cpp src/vtpty/Process.hpp src/vtpty/MockPty.hpp src/vtpty/UnixPty_test.cpp src/vtpty/UnixUtils_test.cpp
git commit -m "vtpty: a PTY says whether the terminal echoes, hides or leaves typed text to the program" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 2: `ISecureInput`, the balanced gate, and the Cocoa backend

**Files:**
- Create: `src/contour/platform/SecureInput.hpp` (interface, `NullSecureInput` inline) and `SecureInput.cpp` (the
  non-Apple `makeSecureInput()`, wholly inside `#ifndef __APPLE__`); list both in `add_library(contour_platform …)`.
- Create: `src/contour/platform/SecureInputGate.hpp` (header-only, Qt-free); list it there too.
- Create: `src/contour/platform/CocoaSecureInput.mm` (ARC; the Apple `makeSecureInput()`); add it to the `if(APPLE)`
  `target_sources` and the `-fobjc-arc` list, and link `"-framework Carbon"` beside AppKit (`:100`).
- Modify: `src/contour/platform/AppearanceServices.hpp` — a `secureInput` factory, named in `nullAppearanceServices()`;
  `src/contour/test/AppearanceFixtures.hpp` — `appearanceServicesWith` names it (null).
- Create: `src/contour/test/SecureInputFixtures.hpp` — `RecordingSecureInput`.
- Test: new `src/contour/platform/SecureInputGate_test.cpp` (contour_test), new
  `src/contour/platform/CocoaSecureInput_test.mm` (contour_gui_test's `if(APPLE)` block, `-fobjc-arc`),
  `src/contour/platform/AppearanceSources_test.cpp` (one case).

**Interfaces:**
- Produces (namespace `contour::platform`, `SecureInput.hpp`):

```cpp
/// Whether the OS has a secure keyboard entry mode at all.
enum class SecureInputSupport : uint8_t { None = 0, Native };

/// Whether secure keyboard entry is on.
enum class SecureInputState : uint8_t { Off = 0, On };

/// The OS's secure keyboard entry: while on, no other process can read the keys typed into this one.
/// Called only by SecureInputGate, which never calls enable() twice without a disable() between them.
class ISecureInput
{
  public:
    virtual ~ISecureInput() = default;
    /// @pre GUI (main) thread; currently off.
    virtual void enable() = 0;
    /// @pre GUI (main) thread; currently on.
    virtual void disable() = 0;
    /// @return Whether enable() does anything here.
    [[nodiscard]] virtual SecureInputSupport support() const noexcept = 0;
};

class NullSecureInput final: public ISecureInput { /* no-ops; support() → None; inline */ };

[[nodiscard]] std::unique_ptr<ISecureInput> makeSecureInput();
```

- Produces (`SecureInputGate.hpp`): `class SecureInputGate` — `explicit SecureInputGate(ISecureInput& os);`,
  non-copyable and non-movable; `void apply(SecureInputState desired);` calls `enable()` only on Off→On and
  `disable()` only on On→Off; `[[nodiscard]] SecureInputState state() const noexcept;`; `~SecureInputGate()` calls
  `disable()` when On. Documented: "the only caller of ISecureInput; balanced by construction".
- Produces (`AppearanceServices`): `std::function<std::unique_ptr<ISecureInput>()> secureInput = makeSecureInput;`;
  `nullAppearanceServices()` names `.secureInput = [] { return std::make_unique<NullSecureInput>(); }`.
- Produces (`test/SecureInputFixtures.hpp`, `contour::test`): `RecordingSecureInput final: public ISecureInput` with
  `int enables = 0; int disables = 0; SecureInputSupport supported = SecureInputSupport::Native;`.
- The Cocoa class `CocoaSecureInput` calls `EnableSecureEventInput()` / `DisableSecureEventInput()`, keeps its own
  `SecureInputState`, asserts `NSThread.isMainThread`, and its destructor disables when On (a second line of defence
  behind the gate). `support()` → `Native`.

- [ ] **Step 1: Write the failing tests.**
  - `SecureInputGate_test.cpp`, `[secureinput]`, with a `RecordingSecureInput`:
    - "secureInputGate.enablesOnceForRepeatedOn": `apply(On)` twice → `enables == 1`, `disables == 0`, `state() == On`.
    - "secureInputGate.disablesOnlyWhenOn": `apply(Off)` on a fresh gate → no call; `apply(On); apply(Off); apply(Off)`
      → `enables == 1`, `disables == 1`.
    - "secureInputGate.destructorTurnsItOff" (Review Focus 2): a scoped gate left On → after the scope
      `disables == 1`; one left Off → `disables == 0`.
  - `AppearanceSources_test.cpp`, `[appearance-source]`: `nullAppearanceServices().secureInput()->support() == None`,
    and so for `appearanceServicesWith(…)`.
  - `CocoaSecureInput_test.mm`, `[secureinput][cocoa]`: `makeSecureInput()->support() == Native`; with `before =
    IsSecureEventInputEnabled()`, a gate over it: `apply(On)` → `IsSecureEventInputEnabled()`; destroying the gate →
    `IsSecureEventInputEnabled() == before`.
- [ ] **Step 2: Run them and see them fail.** Run:
  `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test contour_gui_test`. Expected:
  compile error, no `SecureInput.hpp`.
- [ ] **Step 3: Implement** the interface, the null class, both factories, the gate, the Cocoa class, the services
  field (named in both null factories) and the fixture.
- [ ] **Step 4: Run them and see them pass.** Run:
  `out/appleclang-debug/src/contour/contour_test "[secureinput]" && out/appleclang-debug/src/contour/contour_gui_test "[secureinput],[appearance-source]"`.
  Expected: PASS.
- [ ] **Step 5: Lint, format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) src/contour/platform/SecureInput.cpp src/contour/platform/SecureInputGate_test.cpp src/contour/platform/AppearanceSources_test.cpp
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/platform/SecureInput.hpp src/contour/platform/SecureInput.cpp src/contour/platform/SecureInputGate.hpp src/contour/platform/CocoaSecureInput.mm src/contour/platform/CocoaSecureInput_test.mm src/contour/platform/SecureInputGate_test.cpp src/contour/platform/AppearanceServices.hpp src/contour/platform/AppearanceSources_test.cpp src/contour/test/AppearanceFixtures.hpp src/contour/test/SecureInputFixtures.hpp
editorconfig-checker src/contour/platform/CMakeLists.txt src/contour/CMakeLists.txt
git add src/contour/platform/SecureInput.hpp src/contour/platform/SecureInput.cpp src/contour/platform/SecureInputGate.hpp src/contour/platform/CocoaSecureInput.mm src/contour/platform/CocoaSecureInput_test.mm src/contour/platform/SecureInputGate_test.cpp src/contour/platform/AppearanceServices.hpp src/contour/platform/AppearanceSources_test.cpp src/contour/platform/CMakeLists.txt src/contour/test/AppearanceFixtures.hpp src/contour/test/SecureInputFixtures.hpp src/contour/CMakeLists.txt
git commit -m "platform: secure keyboard entry behind an interface, switched only by a balanced gate" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 3: `secure_input: auto | off`

**Files:**
- Create: `src/contour/config/SecureInputMode.hpp` — the enum and its `ConfigEnumInfo` table, as
  `config/ChromeDensity.hpp` (Step 2) is.
- Modify: `src/contour/config/Config.hpp` — `EditionDefaults` (`:1035`) gains `SecureInputMode secureInput;`, both rows
  of `EditionDefaultsTable` name it; `Config` gains
  `ConfigEntry<SecureInputMode, documentation::SecureInput> secureInput { editionDefaults(defaultsEdition).secureInput };`
  beside `inputMethodEditor` (`:1224`).
- Modify: `src/contour/config/Config.cpp` (the global loader beside `input_method_editor`, `:2125`, and the writer),
  `src/contour/config/ConfigDocumentation.hpp` (a `SecureInput` entry beside `InputMethodEditorSupport`, `:833`).
- Test: `src/contour/config/Config_test.cpp` (contour_gui_test).

**Interfaces:**
- Produces (`contour::config`): `enum class SecureInputMode : uint8_t { Off = 0, Auto };`, spellings `"off"` / `"auto"`
  with labels "Off" / "When a password is asked for", `configEnumValues<SecureInputMode>()`, and
  `[[nodiscard]] constexpr std::string_view secureInputModeName(SecureInputMode) noexcept`.
- Documentation text: "Shows a lock while the program in a pane hides what you type (a password prompt) and, on
  macOS, turns on Secure Keyboard Entry while that pane has focus. `auto` or `off`."

- [ ] **Step 1: Write the failing tests** (`[config][secureinput]`):
  - "a Classic file keeps secure input off": a `contour.yml` without `defaults_edition` → `secureInput == Off`.
  - "the generated config turns it on": the path overload over `defaultConfigString()` → `Auto`.
  - "secure_input: off parses and round-trips": a Native2026 file with `secure_input: off` → `Off`; the YAML writer
    emits `secure_input: off`.
  - "an unknown spelling warns and keeps the edition's default": `secure_input: maybe` → the edition default, one
    warning logged.
- [ ] **Step 2: Run them and see them fail.** Run:
  `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_gui_test`. Expected: compile error, no
  `secureInput` member.
- [ ] **Step 3: Implement** the header, the column, the entry, the loader, the writer and the documentation.
- [ ] **Step 4: Run them and see them pass.** Run:
  `out/appleclang-debug/src/contour/contour_gui_test "[config][secureinput],[config]"`. Expected: PASS.
- [ ] **Step 5: Lint, format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) src/contour/config/Config.cpp src/contour/config/Config_test.cpp
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/config/SecureInputMode.hpp src/contour/config/Config.hpp src/contour/config/Config.cpp src/contour/config/ConfigDocumentation.hpp src/contour/config/Config_test.cpp
git add src/contour/config/SecureInputMode.hpp src/contour/config/Config.hpp src/contour/config/Config.cpp src/contour/config/ConfigDocumentation.hpp src/contour/config/Config_test.cpp src/contour/config/CMakeLists.txt
git commit -m "config: secure_input, off in the classic defaults and auto in the new ones" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 4: Sessions read their echo; the manager drives the gate; the controller publishes the lock

> **Depends on Step 1c** (`TerminalSession::noteOutputForTab()` and `TerminalSessionManager::noteSessionOutput`, the
> output path; `WindowController::BadgeRole` and its neighbours) and **Step 2** (`resolvePaneHeader`, read by the
> provider's new invokable).

**Files:**
- Create: `src/contour/window/SecureInputPolicy.hpp` (header-only, Qt-free); list it in `_header_files`.
- Modify: `src/contour/session/TerminalSession.{hpp,cpp}` — `_inputEcho`, `refreshInputEcho()`, the property.
- Modify: `src/contour/session/TerminalSessionManager.{hpp,cpp}` — `_secureInputGate`, `refreshSecureInput()`,
  `noteInputEchoChanged()`.
- Modify: `src/contour/window/WindowController.{hpp,cpp}` — `SecureRole`.
- Modify: `src/contour/window/UiStyleProvider.{hpp,cpp}` — `secureIndicatorSpots()` and `secureInputText`.
- Modify: `src/contour/ContourGuiApp.{hpp,cpp}` — `_secureInput` (declared beside `_taskbarProgressSink`, R-e) and
  `secureInput()`.
- Test: new `src/contour/window/SecureInputPolicy_test.cpp` (contour_test); `session/TerminalSessionManager_test.cpp`,
  `session/TerminalSession_test.cpp` (contour_gui_test).

**Interfaces:**
- Consumes: `vtpty::InputEcho`, `Pty::inputEcho()`, `MockPty::setInputEcho` (Task 1); `ISecureInput`,
  `SecureInputGate`, `SecureInputState`, `RecordingSecureInput` (Task 2); `config::SecureInputMode` (Task 3).
- Produces (`contour::window`, `SecureInputPolicy.hpp`):
  - `enum class SecureIndicator : uint8_t { None = 0, Locked };`
  - `[[nodiscard]] constexpr SecureIndicator secureIndicatorFor(config::SecureInputMode mode, vtpty::InputEcho echo) noexcept;`
    — `Locked` iff `Auto` and `Hidden`.
  - `enum class AppActivity : uint8_t { Background = 0, Foreground };`
  - `[[nodiscard]] constexpr platform::SecureInputState desiredSecureInput(config::SecureInputMode mode, AppActivity app, vtpty::InputEcho focusedEcho) noexcept;`
  - `enum class SpotShown : uint8_t { No = 0, Yes };`
  - `struct SecureIndicatorRule { SpotShown header; SpotShown tabWell; SpotShown lockInHeader; SpotShown lockOnTab; SpotShown bandChip; };`
    and `inline constexpr std::array<SecureIndicatorRule, 4> SecureIndicatorRules` (R-f: header+well → header and
    tab; header only → header; well only → tab; neither → band chip), with
    `[[nodiscard]] constexpr SecureIndicatorRule const& secureIndicatorSpots(SpotShown header, SpotShown tabWell) noexcept;`.
  - `[[nodiscard]] constexpr std::string_view secureInputStateText(platform::SecureInputSupport support) noexcept;`
    → "Secure input on" / "Input hidden".
- Produces (`TerminalSession`): `[[nodiscard]] vtpty::InputEcho inputEcho() const noexcept;` (the last read; `Unknown`
  for a `SessionHosting::Remote` session, which reads nothing); `void refreshInputEcho();` (GUI thread: re-reads the
  device, and on a change emits `secureIndicatorChanged()` and calls `_manager->noteInputEchoChanged(*this)`);
  `Q_PROPERTY(int secureIndicator READ secureIndicator NOTIFY secureIndicatorChanged)` (a `SecureIndicator` value).
  Called from the posted lambda of `noteOutputForTab()`, after every PTY write from key or character input, and on
  focus-in.
- Produces (`TerminalSessionManager`): member `platform::SecureInputGate _secureInputGate` built from
  `_app.secureInput()`; `void noteInputEchoChanged(TerminalSession const& session);` (refreshes the hosting tab's
  `SecureRole` and the gate); `void refreshSecureInput();` (`desiredSecureInput(config mode, _focusedWindow ?
  Foreground : Background, _focusedSession ? _focusedSession->inputEcho() : Unknown)`), called from
  `setFocusedSession`, `setFocusedWindow`, `clearFocusedWindow`, `removeSession`, the config reload and
  `noteInputEchoChanged`.
- Produces (`WindowController`): `SecureRole = Qt::UserRole + <one after the last role at execution>` — 1 when any leaf
  of the tab is `Locked`.
- Produces (`UiStyleProvider`): `Q_INVOKABLE QVariantMap secureIndicatorSpots(bool headerShown, bool tabWellShown) const`
  (QML carve-out; keys `header`, `tab`, `band`, each a bool) and `Q_PROPERTY(QString secureInputText … CONSTANT)`.
- Produces (`ContourGuiApp`): `[[nodiscard]] platform::ISecureInput& secureInput() noexcept;`, built in the
  constructor from `appearanceServices.secureInput()`.

- [ ] **Step 1: Write the failing tests.**
  - `SecureInputPolicy_test.cpp`, `[secureinputpolicy]`: `secureIndicatorFor` over every `(mode, echo)` pair (only
    `(Auto, Hidden)` is `Locked`); `desiredSecureInput` over every triple (only `(Auto, Foreground, Hidden)` is On);
    `SecureIndicatorRules` has one row per `(header, tabWell)` pair and `secureIndicatorSpots` returns the four R-f
    answers; `secureInputStateText` for both supports.
  - `TerminalSessionManager_test.cpp`, `[manager][secureinput]`, a `TestApp` with `RecordingSecureInput` injected
    through `appearanceServicesWith`, `secure_input: auto`, sessions over `MockPty`:
    - "a raw-mode program shows no lock" (Review Focus 1): `setInputEcho(Raw)`, output → `secureIndicator == None`,
      `enables == 0`.
    - "a focused password prompt turns secure input on, once": `Hidden`, output → `Locked`, `enables == 1`; more output
      → still 1.
    - "closing the prompt's pane, its window, or the manager turns it off" (Review Focus 2): three sections, each ends
      with `disables == 1`.
    - "another application in front turns it off": `clearFocusedWindow(id)` → `disables == 1`; `setFocusedWindow(id)`
      → `enables == 2`.
    - "focus between two prompts calls nothing" (Review Focus 3): two windows, both `Hidden`; focus window A then B →
      `enables == 1, disables == 0`; B's echo returns (`Echoed`, output) → `disables == 1`.
    - "a background prompt locks its tab, not the OS" (Review Focus 4): tab 2 `Hidden` while tab 1 is focused →
      tab 2's `SecureRole == 1`, `enables == 0`; activate tab 2 → `enables == 1`.
    - "a remote-hosted session is never locked" (Review Focus 5): a remote-hosted test factory's session →
      `inputEcho() == Unknown`, `secureIndicator == None`.
    - "echo restored without output is noticed on the next key": `Hidden` then `setInputEcho(Echoed)` with no output;
      a key event → `secureIndicator == None`, `disables == 1`.
    - "secure_input: off never calls the OS": `Hidden` and focused → `enables == 0`, `None`.
- [ ] **Step 2: Run them and see them fail.** Run:
  `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test contour_gui_test`. Expected:
  compile error, no `SecureInputPolicy.hpp`.
- [ ] **Step 3: Implement** the policy header, the session's echo, the manager's gate and refresh points, the role,
  the provider answers and the app accessor (R-e's declaration order).
- [ ] **Step 4: Run them and see them pass.** Run:
  `out/appleclang-debug/src/contour/contour_test "[secureinputpolicy]" && out/appleclang-debug/src/contour/contour_gui_test "[manager][secureinput],[manager][tabbadge],[contour][uistyle]"`.
  Expected: PASS.
- [ ] **Step 5: Lint, format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) src/contour/session/TerminalSession.cpp src/contour/session/TerminalSessionManager.cpp src/contour/window/WindowController.cpp src/contour/window/UiStyleProvider.cpp src/contour/ContourGuiApp.cpp src/contour/window/SecureInputPolicy_test.cpp src/contour/session/TerminalSessionManager_test.cpp
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/window/SecureInputPolicy.hpp src/contour/window/SecureInputPolicy_test.cpp src/contour/session/TerminalSession.hpp src/contour/session/TerminalSession.cpp src/contour/session/TerminalSessionManager.hpp src/contour/session/TerminalSessionManager.cpp src/contour/session/TerminalSessionManager_test.cpp src/contour/window/WindowController.hpp src/contour/window/WindowController.cpp src/contour/window/UiStyleProvider.hpp src/contour/window/UiStyleProvider.cpp src/contour/ContourGuiApp.hpp src/contour/ContourGuiApp.cpp
git add src/contour/window/SecureInputPolicy.hpp src/contour/window/SecureInputPolicy_test.cpp src/contour/session/TerminalSession.hpp src/contour/session/TerminalSession.cpp src/contour/session/TerminalSessionManager.hpp src/contour/session/TerminalSessionManager.cpp src/contour/session/TerminalSessionManager_test.cpp src/contour/window/WindowController.hpp src/contour/window/WindowController.cpp src/contour/window/UiStyleProvider.hpp src/contour/window/UiStyleProvider.cpp src/contour/ContourGuiApp.hpp src/contour/ContourGuiApp.cpp src/contour/CMakeLists.txt
git commit -m "contour: a pane at a password prompt is locked, and the focused one turns secure input on" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 5: The lock on the tab, in the pane header and in the band; snapshots; the Terminal row

> **Depends on Step 1c** (`TabItem.qml` pills, `TitleBar.qml`'s band and its chips), **Step 2** (`PaneHeader.qml` and
> the provider's pane-header answer), **Step 5b** (`src/contour/res/icons/` and the `/contour/icons/` resource prefix,
> tinted with `icon.color`) and **Step 1e** (the snapshot driver).

**Files:**
- Create: `src/contour/res/icons/lock-symbolic.svg` (16 × 16 canvas around the board's 12 × 12 outline,
  `fill`/`stroke="currentColor"`); add it to `resources.qrc` under `/contour/icons/`.
- Create: `src/contour/qml/SecureInputLock.qml` (add to `_qml_files`): an `IconLabel`-based item with `icon.name:
  "changes-prevent-symbolic"`, `icon.source: "qrc:/contour/icons/lock-symbolic.svg"`, `icon.color` the header's
  secondary label colour, edge `chromeStyle.size.secureInputGlyph`; on the Terminal row it draws
  `chipOpenGlyph + secureInputText + chipCloseGlyph` as text. `Accessible.role: Accessible.Indicator`,
  `Accessible.name: chromeStyle.secureInputText`. `objectName: "secureInputLock"`.
- Modify: `src/contour/qml/TabItem.qml` (the lock before the title when the `tabSecure` role is 1 and the spots say
  `tab`), `PaneHeader.qml` (the lock, then `<process> · <text>` in place of the title while locked), `TitleBar.qml`
  (a `secureInputChip`, a toolbar chip from 1c's chip tokens, shown when the spots say `band`).
- Modify: `src/contour/config/ChromeTokenGroups.hpp` (`size.secureInputGlyph`, four rows), `config/UiStyle.hpp`
  (`secureInputText`, both rows), `src/contour/window/ChromeTokenObjects.{hpp,cpp}` (publish the token).
- Modify: `src/contour/test/ChromeSnapshotPlan.hpp`, `MockMainController.hpp`, `QmlMocks.hpp` — two scripts.
- Test: `src/contour/test/QmlComponents_test.cpp`, `MainWindowQml_test.cpp`, `Accessibility_qml_test.cpp`,
  `ChromeSnapshotPlan_test.cpp`, `config/ChromeTokenGroups_test.cpp`.

**Interfaces:**
- Consumes: `SecureRole`, `secureIndicator`, `secureIndicatorSpots`, `secureInputText` (Task 4).
- Produces (snapshots): `SnapshotScript::SecureInput` (`"secure-input"`: two tabs, `PaneShape::SplitThree`, the
  active pane and tab 2 locked; every other field as the row then stands) and `SnapshotScript::SecureInputBand`
  (`"secure-input-band"`: one tab, one pane, locked). Mock state: `MainWindowScene` gains
  `SecureDecor { None = 0, Locked }` per scene.

- [ ] **Step 1: Write the failing tests.**
  - `ChromeTokenGroups_test.cpp`, `[chrometokens]`: the row pin names `size.secureInputGlyph` (11, 11, 11, 0).
  - `QmlComponents_test.cpp`, `[contour][gui][qml][secureinput]`: `SecureInputLock` loads with no QML warning; its
    `Accessible.name` is the provider's text; on the MacOS row its icon edge equals `chromeStyle.size.secureInputGlyph`.
  - `MainWindowQml_test.cpp`, `[contour][gui][qml][secureinput]`: two tabs with tab 2 locked → `secureInputLock`
    visible inside tab 2's `TabItem`, not tab 1's; one tab, one pane, locked → the band's `secureInputChip` visible
    and no lock on any tab; a split with a locked pane → its `PaneHeader`'s lock visible and its text
    `"<process> · Secure input on"` under a `Native` support.
  - `Accessibility_qml_test.cpp`: the band chip and the tab lock appear in the accessible tree with the text.
  - `ChromeSnapshotPlan_test.cpp`, `[snapshot]`: both scripts exist with their names and shapes.
- [ ] **Step 2: Run them and see them fail.** Run:
  `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test contour_gui_test`. Expected:
  compile error, no `SnapshotScript::SecureInput`.
- [ ] **Step 3: Implement** the icon, the component, the three placements, the tokens and the scripts. Regenerate the
  touched files' size baselines with `python3 scripts/check-qml-sizes.py --print-baseline` (new file 0).
- [ ] **Step 4: Run them and see them pass, on both rows.** Run:

```bash
out/appleclang-debug/src/contour/contour_test "[chrometokens],[snapshot]"
out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][secureinput],[a11y],[contour][gui][qml]"
CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][secureinput]"
ctest --test-dir out/appleclang-debug -R "check_qml" --output-on-failure
```

  Expected: PASS; under ContourTui the lock is the `[secure]` text, one cell row high.
- [ ] **Step 5: Lint, format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) src/contour/window/ChromeTokenObjects.cpp src/contour/test/QmlComponents_test.cpp src/contour/test/MainWindowQml_test.cpp
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/config/ChromeTokenGroups.hpp src/contour/config/UiStyle.hpp src/contour/config/ChromeTokenGroups_test.cpp src/contour/window/ChromeTokenObjects.hpp src/contour/window/ChromeTokenObjects.cpp src/contour/test/ChromeSnapshotPlan.hpp src/contour/test/ChromeSnapshotPlan_test.cpp src/contour/test/MockMainController.hpp src/contour/test/QmlMocks.hpp src/contour/test/QmlComponents_test.cpp src/contour/test/MainWindowQml_test.cpp src/contour/test/Accessibility_qml_test.cpp
editorconfig-checker src/contour/qml/SecureInputLock.qml src/contour/qml/TabItem.qml src/contour/qml/PaneHeader.qml src/contour/qml/TitleBar.qml src/contour/res/icons/lock-symbolic.svg src/contour/resources.qrc scripts/qml-size-literal-baseline.txt src/contour/CMakeLists.txt
git add src/contour/res/icons/lock-symbolic.svg src/contour/resources.qrc src/contour/qml/SecureInputLock.qml src/contour/qml/TabItem.qml src/contour/qml/PaneHeader.qml src/contour/qml/TitleBar.qml src/contour/config/ChromeTokenGroups.hpp src/contour/config/UiStyle.hpp src/contour/config/ChromeTokenGroups_test.cpp src/contour/window/ChromeTokenObjects.hpp src/contour/window/ChromeTokenObjects.cpp src/contour/test/ChromeSnapshotPlan.hpp src/contour/test/ChromeSnapshotPlan_test.cpp src/contour/test/MockMainController.hpp src/contour/test/QmlMocks.hpp src/contour/test/QmlComponents_test.cpp src/contour/test/MainWindowQml_test.cpp src/contour/test/Accessibility_qml_test.cpp scripts/qml-size-literal-baseline.txt src/contour/CMakeLists.txt
git commit -m "contour: a lock on the tab, in the pane header or in the band while typing is hidden" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 6: Finish

- [ ] **Step 1:** Run `/simplify` on the step's diff, `git diff <the plan commit>..HEAD -- src scripts`, apply its
  findings, re-run the filters of every task it touches, and commit once with the Signed-off-by line, staging only the
  files it changed.
- [ ] **Step 2:** Run every task's filter once more:

```bash
out/appleclang-debug/src/vtpty/vtpty_test "[inputecho]"
out/appleclang-debug/src/contour/contour_test "[secureinput],[secureinputpolicy],[chrometokens],[snapshot]"
out/appleclang-debug/src/contour/contour_gui_test "[secureinput],[appearance-source],[config][secureinput],[manager][secureinput],[manager][tabbadge],[contour][uistyle],[contour][gui][qml],[a11y]"
CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][secureinput]"
ctest --test-dir out/appleclang-debug -L lint --output-on-failure
```

  Expected: PASS.
- [ ] **Step 3: Snapshots, on this Mac's display** (windows flash; CI runs them under weston):

```bash
export CONTOUR_SNAPSHOT_DIR="$PWD/out/chrome-snapshots"; rm -rf "$CONTOUR_SNAPSHOT_DIR"
CONTOUR_TEST_DISPLAY=1 out/appleclang-debug/src/contour/contour_gui_test "[display][snapshot]"
CONTOUR_TEST_DISPLAY=1 CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[display][snapshot]"
ls "$CONTOUR_SNAPSHOT_DIR"/*.png | wc -l
```

  Expected: the count equals `SnapshotScripts.size() × 4 rows × 2 editions × 2 appearances`, the size read from
  `ChromeSnapshotPlan.hpp`. Look at `secure-input.*` (a lock on tab 2 and in the active card's header) and
  `secure-input-band.*` (the band chip); under ContourTui both read `[secure]`.
- [ ] **Step 4: Owner manual checks**, batched per phase (listed in the ledger, not awaited), on
  `out/appleclang-debug/src/contour/contour.app` with a fresh (Native2026) config:
  - `sudo -k; sudo true`: the lock appears with the prompt; `ioreg -l -w 0 | grep SecureInput` names Contour's PID;
    after the password it is gone and the PID with it.
  - `vim`, `htop`, `less /etc/hosts`, and the bare zsh prompt: no lock.
  - At the `sudo` prompt, ⌘Tab to another app: the PID leaves `ioreg`; ⌘Tab back: it returns. Close the tab at the
    prompt: it leaves. `kill -9` Contour at the prompt: it leaves (R-d).
  - `defaults_edition: classic`: no lock, no secure input.
- [ ] **Step 5: Release notes** with `/add-release-note` in `metainfo.xml`, under the open `0.9.0` release: "Shows a lock
  on the tab or pane while a program hides what you type, such as a password prompt, and turns on Secure Keyboard
  Entry on macOS for as long as that pane has focus; adds `secure_input` (`auto`, `off`)."
- [ ] **Step 6:** Mark Step 6.6 done in the plans index's step table and its *Last update* line, and on the board
  (`/status-board update`). Copy the SDD ledger to `docs/superpowers/ledgers/2026-10-25-native-redesign-step-6-6.md`.
  Update the PR body with `/update-pr`, reporting:
  - **Performance:** one `tcgetattr` per coalesced output burst, per key written and per focus-in, on the GUI thread;
    nothing on the parser or render paths.
  - **Risk:** low to medium. The OS state is process-wide; it is switched only by the gate, from one place, and every
    teardown path is tested.
  - **Coverage:** `[inputecho]` (with a real PTY), `[secureinput]`, `[secureinputpolicy]`, `[manager][secureinput]`,
    the Cocoa round trip, the QML cases on both rows, two snapshot scripts.

---

## Owner decisions

The run proceeds on each recommended default.

1. **Classic users (R-g).** *Default: `secure_input` is `off` in Classic and `auto` in Native2026.* The alternative turns
   it on for everyone: a security gain, but a lock existing users have never seen, and other apps' global hotkeys stop
   while it is on.
2. **The words off macOS.** *Default: "Secure input on" where the OS has it (macOS), "Input hidden" elsewhere*, since
   Windows and Linux have no secure keyboard entry to turn on. The alternative says "Password" everywhere.
3. **Remote panes (R-h).** *Default: no lock for daemon- and tmux-mirrored panes until the protocol carries the echo
   state.* The alternative adds a Delta field in this step.

## Spec gaps found

- §3.16 says "when echo is off"; read literally, every shell prompt and editor would lock (R-a).
- No source is given for when to read the echo, or how secure input relates to focus and app activation (R-b, R-c).
- The lock's placement when neither a pane header nor the tab well is shown is not drawn (R-f's band chip).
- No Classic value and no config key are named (R-g); the canvas's "Secure input on" text has no non-macOS form.
- The daemon protocol has no echo field (R-h).
