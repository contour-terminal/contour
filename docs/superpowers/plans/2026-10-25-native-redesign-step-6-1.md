# Native Redesign — Step 6.1 (Moments: the Shell Integration Card) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

Written ahead (pipelining ruling). Line numbers are at HEAD `661b6029`, before 1c (in progress), 1d, 2, 3a–3f,
4b, 5a and 5b land. Where this step uses code they add, the task names it as their plans do and carries a
**Depends on Step <id>** note. A pre-flight scan re-verifies every reference before 6.1 executes.

**Goal:** After three commands in a local bash, zsh or fish pane that never wrote an OSC 133 mark, the pane shows
one dismissible card; Install shows the exact line and the rc file it goes into, and only a second press appends
it there, once.

**Architecture:**
- **Pure decisions, Qt-free.** `config/ShellKind.hpp` names the three shells and where each keeps its rc file.
  `window/ShellIntegrationOffer.hpp` holds the probe (commands counted until a prompt mark is seen), the offer
  decision, the card's stages and copy, and the one line it appends (`shellRcLine`). All table-tested in
  `contour_test`.
- **The rc file through one seam.** `platform::IShellRcEditor` (`inspect`, `install`) has an inline
  `NullShellRcEditor` and a `FileShellRcEditor` over the injected `core::platform::FileSystem` and
  `core::Environment`. It only ever appends. `ContourGuiApp` owns one; every `TestApp` gets a recording one, so no
  test can reach a real `~/.zshrc`.
- **The session decides, QML renders.** `TerminalSession` feeds the probe from Return presses and asks the
  manager to offer; the manager enforces one card per run, the user's "Not Now" (in `gui-state.yml`) and the rc
  state. `ShellIntegrationCard.qml` sits in the pane card's bottom chrome (Step 2's footer slot), never over cells.
- **Settings** gains the *Install* group of the Shell Integration section (Step 4b R-m left it to 6.1): one row
  per shell with its state and an Install… button that shows the same line first.

**Tech Stack:** C++23, Qt 6.11 (Quick, Quick Controls), QML, Catch2 v3, CMake build tree `out/appleclang-debug`.

**Spec:** `docs/superpowers/specs/2026-10-24-native-redesign-implementation-design.md` §3.16 row 6.1 (`:818`):
"After 3 prompts without OSC 133, a dismissible card. Install appends the one-line source to the rc file, showing
it first, using `contour generate integration`", platform seam `IShellRcEditor`. It binds with what it cites: §1
principles (`:28-64`; 3 pure decisions, 4 the OS through interfaces, 5 nothing over non-blank cells), §2.1 token
rows (`:68-126`), §2.6 testing (`:206-226`), §2.7 existing users (`:228-284`), §5 (`:844-854`). The design record
is `docs/superpowers/specs/2026-10-23-native-redesign-design.md` ("Moments: first run, restore, quick terminal,
and similar"). The canvas is <https://claude.ai/artifact/12ASSTRD5ua5SwarsMJXwm>; its local copies are in
`.superpowers/design/canvas/project/`: `moments` board 2 (the card), `kit-components`, `mac-settings` ("Shell
Integration has its own section: install status per shell, …"), `tui`. The plans index is
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

- **The spec, verbatim (§3.16 row 6.1):** "After 3 prompts without OSC 133, a dismissible card. Install appends
  the one-line source to the rc file, showing it first, using `contour generate integration`"; seam
  "`IShellRcEditor`". From the brief: file I/O through DI; never edit an rc file without the user's press; detect
  bash/zsh/fish; idempotent.
- **Canvas values, exact** (`moments` 2): card 330 wide, radius 12, padding 12 × 14, gap 8; title 13 semibold
  "Turn on command blocks"; body 12 "Install shell integration for zsh to fold output, see exit codes and jump
  between commands."; buttons 24 high, radius 6, "Not Now" (fill) and "Install" (accent); bottom-centred.
- **Binding rulings** (ledgers in `docs/superpowers/ledgers/`, `.superpowers/sdd/*/progress.md`):
  - Every OS or file service is an interface with a null implementation, injected at construction (§1.4); TestApp
    is Classic (R14) and passes `nullAppearanceServices()`.
  - No colour or size literal in chrome QML (`check_qml_colors`, `check_qml_sizes`; a new file's baseline is 0).
  - Every visual surface is a snapshot row (`ChromeSnapshotPlan.hpp`), and every visual task renders the Terminal
    row (`CONTOUR_TEST_CONTROLS_STYLE=ContourTui`).
  - `[display]` tests are opt-in (`CONTOUR_TEST_DISPLAY=1`) and skip offscreen.
  - `enum class` over `bool`, the default enumerator at zero; QML-facing `Q_PROPERTY`/`Q_INVOKABLE` booleans are the
    documented carve-out. `std::expected` with a subsystem error enum starting at `= 0`.
  - Any new default goes through `DefaultsEdition`'s table (`EditionDefaults`, `Config.hpp:1035`). This step adds
    no config key (R-k).
  - Text contrast at least 4.5:1 against contour-dark and contour-light. Every new control sets `Accessible.role`
    and `Accessible.name`.
  - Owner manual checks are batched per phase: listed in the ledger, never awaited.
- **AGENT.md applies in full:** data-driven tables (`ShellRows`, `CardStageRows`, the new `NoticeRows` rows);
  dependency injection (the editor, the file system, the environment, the binary path); configuration at
  construction; Doxygen on every new public declaration; `.hpp` with `#pragma once`; zero warnings and no `NOLINT`;
  `slots`, `signals` and `emit` are Qt macros, never identifiers.
- **Build** with every core; the tree is configured with `-DCONTOUR_EXTENSIONS_BUNDLED=OFF`:
  `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target <target>`. Never run two builds at once.
- **Lint and format** before each commit: `/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug
  --extra-arg=-isysroot$(xcrun --show-sdk-path) <touched .cpp files>` (no new diagnostics);
  `/opt/homebrew/opt/llvm@22/bin/clang-format -i` on touched `.cpp`/`.hpp`; `editorconfig-checker` on touched
  `.qml`, `.txt`, `CMakeLists.txt`. **Never clang-format a `.qml` file.**
- **Tests:** each task runs only its own filter; the full suite runs in CI on Linux, macOS and Windows.
- **CI's clang 22 enforces `-Wmissing-designated-field-initializers`:** every new struct member gets `{}` or a
  value, and every new table row names every field (`ShellRows`, `CardStageRows`, `NoticeRows`,
  `ChromeTokenRows`, `SnapshotScripts`).
- **Header-only functions are `inline` or `constexpr`.** Counts in tests come from tables (`ShellRows.size()`,
  `SnapshotScripts.size()`), never literals.
- **Qt-free stays Qt-free:** `config/ShellKind.hpp`, `window/ShellIntegrationOffer.hpp` and
  `platform/ShellRcEditor.hpp` include no Qt header.
- **Never edit `vendor/core-cpp`.**
- **Commits:** one per task. Stage only that task's files, by name (other agents share the tree; never `git add -A`
  or a directory). Each message ends with exactly `Signed-off-by: Christian Parpart <christian@parpart.family>`,
  and never carries `Co-Authored-By`.

## Review Focus

1. **A test run, or CI, appends to the developer's real `~/.zshrc`.** Expected: no test can reach a file editor
   over the real file system; `TestApp` holds a recording editor by default, and the file editor is exercised only
   over `InMemoryFileSystem`. Pinned in Task 3 ("TestApp never reaches a file editor") and Task 2.
2. **Install pressed twice, or an rc that already sources Contour's script by hand.** Expected: the line is
   appended at most once; an rc that holds the marker, a `generate integration shell <name>` line, or a `source`
   of `shell-integration.<name>` counts as installed, and installing again writes nothing. Pinned in Task 2
   (`[shellrc]` "a second install writes nothing", "a manual install counts").
3. **The card names the wrong shell or the wrong machine.** The user typed `bash` inside zsh, or `ssh host`, or the
   window is attached to a remote host. Expected: the shell is the PTY's foreground process at the third Return;
   `ssh`, `tmux`, `sudo` and anything not bash/zsh/fish offer nothing; a remote-hosted window offers nothing. Pinned
   in Task 3 ("ssh in the foreground offers nothing", "a remote-hosted window offers nothing").
4. **A binary path with spaces or quotes** (`/Applications/Contour Nightly.app/…`, a home named `O'Brien`).
   Expected: the line quotes the path for the shell's syntax and every new shell still starts; a path holding a
   control character is refused, never written. Pinned in Task 1 (`shellRcLine` quoting cases).
5. **An rc file without a trailing newline, or a symlinked dotfile** (stow, a dotfiles repo). Expected: the line
   starts on its own line; the editor only appends (through the link to its target), never rewrites or replaces
   the file. Pinned in Task 2 ("no trailing newline", "only ever appends").

Also pinned, beyond the five: a vim session's Returns do not count (Task 3); bash on macOS goes into
`~/.bash_profile` (Task 2); a failed write keeps the card open with the reason (Task 3).

---

## Spec resolutions (read before Task 1)

Those marked **(owner)** are repeated under *Owner decisions*; the run proceeds on their default.

- **R-a. "3 prompts without OSC 133" is three commands entered.** A prompt without OSC 133 cannot be seen; that is
  what OSC 133 marks. The canvas says "after three commands". A command is a Return press (no modifier) that the
  session writes to the PTY on the primary screen. Returns on the alternate screen (vim, less) do not count.
  The probe ends for the session's life as soon as the terminal holds a command-block record (any OSC 133 mark
  creates one; a restored pane's adopted records count too). **Pre-flight:** confirm OSC 133 `A` alone creates a
  record in `CommandBlockStore`; if not, read the terminal's prompt-mark state instead.
- **R-b. Which shell.** The PTY's foreground process name at the third counted Return (Step 2's
  `vtpty::Pty::foregroundProcessName()`), its basename with a leading `-` (login shell) dropped, matched exactly
  against `bash`, `zsh`, `fish` (owner: tcsh and pwsh have scripts but the brief names three). Anything else offers
  nothing.
- **R-c. This machine only.** A window attached to a remote host (`ContourGuiApp::attachedHostLabel()` not empty)
  offers nothing: its shells read another machine's rc files. A local daemon's sessions run here and do offer.
  A Windows host offers nothing (no bash/zsh/fish rc convention there).
- **R-d. The line (owner).** One line, guarded so only shells started by Contour run it, with the running binary's
  absolute path (`QCoreApplication::applicationFilePath()`, handed in by the composition root), ending in the
  marker `# Contour shell integration`:
  - bash and zsh: `if [ "$TERM_PROGRAM" = contour ]; then eval "$('<bin>' generate integration shell <name> to -)"; fi # Contour shell integration`
  - fish: `if test "$TERM_PROGRAM" = contour; '<bin>' generate integration shell fish to - | source; end # Contour shell integration`
  - Quoting: POSIX `'` becomes `'\''`; fish `\` becomes `\\` and `'` becomes `\'`. A relative path, or one holding
    NUL or another control character, is refused (`ShellRcLineError`).
- **R-e. Which rc file.** zsh: `$ZDOTDIR/.zshrc`, else `$HOME/.zshrc`. bash: `$HOME/.bash_profile` on macOS
  (Terminal-style login shells read it, not `.bashrc`), `$HOME/.bashrc` elsewhere (owner). fish:
  `$XDG_CONFIG_HOME/fish/config.fish`, else `$HOME/.config/fish/config.fish`.
- **R-f. Idempotent, append-only.** Installed means the file holds `ShellRcMarker`, or a line containing
  `generate integration shell <name>`, or one that sources a file whose name is `shell-integration.<name>` (the
  documented manual install, `docs/vt-extensions/osc-133-shell-integration.md:12-15`); for fish also an existing
  `conf.d/contour.fish`. Installing an installed file returns `AlreadyInstalled` and writes nothing. Otherwise the
  editor creates missing parent directories, and calls `appendFile` once with `"\n"` first when the file is
  non-empty and does not end in a newline, then the line and `"\n"`. It never calls `writeFile`, `rename` or
  `replaceFile` on the rc path, so a symlinked rc is appended through to its target.
- **R-g. The press.** The card's Install moves it to *Confirm*: "Add this line to ~/.zshrc?" with the exact line,
  selectable, and "Add Line" · "Cancel". Only "Add Line" writes. Settings' Install… opens a `KitDialog` with the same
  Confirm copy.
- **R-h. Not Now.** It is remembered per shell in `gui-state.yml` (`dismissed_shell_integration: [zsh]`); that shell
  is never offered again by a card. Settings still installs it. At most one card shows per run of the app, in the
  first session that crosses the threshold; Cancel on Confirm hides it for the run without remembering.
- **R-i. Placement (owner).** The card sits in the pane card's bottom chrome, where Step 2's `ExitedFooter` sits,
  centred, and the terminal gives up those rows while it shows: §1.5 forbids covering non-blank cells, and after
  three commands the bottom rows may hold output. An ended pane's footer wins: the card hides.
- **R-j. After the write.** The card hides and a toast says "Added shell integration to ~/.zshrc. New zsh sessions
  show command blocks." (new `NoticeKind::ShellIntegrationAdded`, 4000 ms). A failure keeps the card on Confirm with
  "~/.zshrc was not changed: {describe(error)}." under the line, and posts the same text as
  `NoticeKind::ShellIntegrationFailed` (6000 ms, glyph `!`). The running shell is not touched (owner).
- **R-k. Both editions, no config key (owner).** The card is a one-time offer, not a look §2.7 protects.
- **R-l. Tokens.** New component tokens: `size.noticeCard` (330 on MacOS, Fluent, ContourLinux; Terminal 0 = the
  pane's width less two cells), `radius.noticeCard` (12 · 8 · 12 · 0), `space.noticeCardPadX` (14 · 14 · 14 · 1),
  `space.noticeCardPadY` (12 · 12 · 12 · 0). Gap `s4`. Title `type.body` at weight 600; body `type.tab` in
  `label.secondary`; buttons `size.footerButton` (Step 2) at `radii.control`; Install accent with
  `onStatusColor(Running)` text; the other `fill(3)`. Card fill: the popover surface role with `elevation.e2`.
  Terminal row: a box of cells (title row, body row, button row with `chipOpenGlyph`/`chipCloseGlyph` brackets).

### Non-goals

- tcsh and PowerShell (owner 4); the quick-terminal hotkey row (6.7); sourcing the script into the running shell.

## File structure

| File | Responsibility | Task |
|---|---|---|
| `src/contour/config/ShellKind.hpp` (create) | `ShellKind`, `RcState`, `ShellRows`, `shellKindOf` | 1 |
| `src/contour/window/ShellIntegrationOffer.hpp` (create) | Probe, offer, card stages and copy, `shellRcLine`, `displayRcPath` | 1 |
| `src/contour/platform/ShellRcEditor.hpp`, `.cpp` (create) | `IShellRcEditor`, `NullShellRcEditor`, `FileShellRcEditor` | 2 |
| `src/contour/config/GuiStateStore.{hpp,cpp}` (modify) | `dismissed_shell_integration` | 2 |
| `src/contour/session/TerminalSession.{hpp,cpp}`, `TerminalSessionManager.{hpp,cpp}`, `ContourGuiApp.{hpp,cpp}`, `window/NoticeQueue.hpp` (modify) | Probe feeding, card state, install, notices | 3 |
| `src/contour/test/RecordingShellRcEditor.hpp` (create), `test/GuiTestFixtures.hpp` (modify) | The test double | 3 |
| `src/contour/qml/ShellIntegrationCard.qml` (create), `qml/PaneCard.qml`, token files, `test/QmlMocks.hpp`, snapshot plan (modify) | The card | 4 |
| `src/contour/window/SettingsController.{hpp,cpp}`, the Shell Integration page QML (modify) | The *Install* group | 5 |

---

### Task 1: The shells and the offer, as data

**Files:**
- Create: `src/contour/config/ShellKind.hpp` (list it in `contour_config`'s sources).
- Create: `src/contour/window/ShellIntegrationOffer.hpp`; test `src/contour/window/ShellIntegrationOffer_test.cpp` in
  `contour_test` (beside `window/TabBadge_test.cpp`).

**Interfaces:**
- Produces (`config/ShellKind.hpp`, namespace `contour::config`):
  - `enum class ShellKind : uint8_t { Unknown = 0, Bash, Zsh, Fish };`
  - `enum class RcState : uint8_t { Unknown = 0, Absent, Installed };`
  - `enum class RcSyntax : uint8_t { Posix = 0, Fish };`, `enum class RcBase : uint8_t { Home = 0, ZDotDir, XdgConfig };`
  - `struct ShellRow { ShellKind kind; std::string_view name; RcSyntax syntax; RcBase base; std::string_view rcFile; std::string_view rcFileMacOS; };`
    `inline constexpr auto ShellRows = std::array { … };` in enumerator order from `Bash`:
    `{Bash, "bash", Posix, Home, ".bashrc", ".bash_profile"}`, `{Zsh, "zsh", Posix, ZDotDir, ".zshrc", ".zshrc"}`,
    `{Fish, "fish", Fish, XdgConfig, "fish/config.fish", "fish/config.fish"}`.
  - `[[nodiscard]] constexpr ShellKind shellKindOf(std::string_view processName) noexcept;` — basename after the last
    `/`, one leading `-` dropped, exact match on `name`; else `Unknown`.
  - `[[nodiscard]] constexpr ShellRow const* shellRow(ShellKind kind) noexcept;` (nullptr for `Unknown`).
  - `inline constexpr std::string_view ShellRcMarker = "# Contour shell integration";` — here, not in `window/`, so
    Task 2's editor can read it.
- Produces (`window/ShellIntegrationOffer.hpp`, namespace `contour::window`):
  - `inline constexpr int ShellIntegrationProbeCommands = 3;`
  - `enum class PromptMarks : uint8_t { Absent = 0, Seen };`, `enum class ProbeVerdict : uint8_t { Waiting = 0, Offer, Integrated };`
  - `class ShellIntegrationProbe { public: [[nodiscard]] ProbeVerdict noteCommandEntered(vtbackend::ScreenType screen, PromptMarks marks) noexcept; };`
    — `Integrated` for every call once `marks` was `Seen`; `Offer` exactly once, on the
    `ShellIntegrationProbeCommands`-th primary-screen call with `Absent`; `Waiting` otherwise.
  - `enum class RcLocality : uint8_t { Remote = 0, ThisMachine };`, `enum class OfferDismissal : uint8_t { Open = 0, Dismissed };`,
    `enum class CardSlot : uint8_t { Free = 0, Taken };`, `enum class ShellIntegrationOffer : uint8_t { None = 0, Card };`
  - `struct OfferInputs { config::ShellKind shell = config::ShellKind::Unknown; RcLocality locality = RcLocality::Remote; config::HostPlatform host = config::HostPlatform::Other; OfferDismissal dismissal = OfferDismissal::Open; config::RcState rc = config::RcState::Unknown; CardSlot slot = CardSlot::Free; };`
    `[[nodiscard]] constexpr ShellIntegrationOffer offerFor(OfferInputs const& in) noexcept;` — `Card` iff the shell
    is known, `ThisMachine`, the host is not `Windows`, `Open`, `Absent` and `Free`.
  - `enum class CardStage : uint8_t { Hidden = 0, Offer, Confirm };`
    `struct CardStageRow { CardStage stage; std::string_view title; std::string_view body; std::string_view primary; std::string_view secondary; };`
    `inline constexpr auto CardStageRows = std::array { … };` (enumerator order; `Hidden` all empty):
    `Offer`: `"Turn on command blocks"`, `"Install shell integration for {shell} to fold output, see exit codes and jump between commands."`, `"Install"`, `"Not Now"`;
    `Confirm`: `"Add this line to {rc}?"`, `"{line}"`, `"Add Line"`, `"Cancel"`.
    `[[nodiscard]] std::string cardText(std::string_view pattern, std::string_view shell, std::string_view rc, std::string_view line);`
  - `enum class ShellRcLineError : uint8_t { UnknownShell = 0, RelativePath, UnusablePath };`
    `[[nodiscard]] std::expected<std::string, ShellRcLineError> shellRcLine(config::ShellKind shell, std::filesystem::path const& contourBinary);` — R-d.
  - `[[nodiscard]] std::string displayRcPath(std::filesystem::path const& rc, std::filesystem::path const& home);` — `~/`
    for a path under `home`, else the path.

- [ ] **Step 1: Write the failing tests** (`[shellintegration]`):
  - `ShellRows` is in enumerator order from `Bash` (`static_assert` plus a loop over `ShellRows.size()`); every row's
    `name` is in `cli::supportedShells()` (so `generate integration` knows it).
  - `shellKindOf`: `"zsh"`, `"-zsh"`, `"/bin/zsh"` → `Zsh`; `"/usr/local/bin/fish"` → `Fish`; `"ssh"`, `"zsh5"`,
    `"tmux"`, `""` → `Unknown`.
  - The probe: three `Primary`/`Absent` calls → `Waiting, Waiting, Offer`; a fourth → `Waiting`; `Alternate` calls
    never count; a `Seen` call at any point → `Integrated` then and after.
  - `offerFor`: every field flipped once from the all-yes input gives `None`.
  - `shellRcLine(Zsh, "/Applications/Contour.app/Contents/MacOS/contour")` equals
    `if [ "$TERM_PROGRAM" = contour ]; then eval "$('/Applications/Contour.app/Contents/MacOS/contour' generate integration shell zsh to -)"; fi # Contour shell integration`;
    the fish form for `Fish`. (Review Focus 4) `"/Applications/Contour Nightly.app/x"` keeps the space inside the
    quotes; `"/home/O'Brien/contour"` gives `'/home/O'\''Brien/contour'` (Posix) and `'/home/O\'Brien/contour'`
    (Fish); `"contour"` → `RelativePath`; a path holding `"\n"` → `UnusablePath`; `Unknown` → `UnknownShell`.
  - `cardText(CardStageRows[Offer].body, "zsh", …)` is the canvas body; `displayRcPath("/Users/me/.zshrc", "/Users/me") == "~/.zshrc"`.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test`

  Expected: compile error, `window/ShellIntegrationOffer.hpp` not found.
- [ ] **Step 3: Implement** both headers as stated.
- [ ] **Step 4: Run them and see them pass.**

  Run: `out/appleclang-debug/src/contour/contour_test "[shellintegration]"`

  Expected: PASS.
- [ ] **Step 5: Lint, format and commit** the files above by name:
  `git commit -m "contour: the shells Contour integrates and the offer to install it, as data" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"`.

---

### Task 2: `IShellRcEditor` and the remembered dismissal

**Files:**
- Create: `src/contour/platform/ShellRcEditor.hpp`, `.cpp` (list both in `add_library(contour_platform …)`); test
  `src/contour/platform/ShellRcEditor_test.cpp` in `contour_test`.
- Modify: `src/contour/config/GuiStateStore.hpp`, `.cpp` (`GuiState` and its YAML); test
  `src/contour/config/GuiStateStore_test.cpp`.

**Interfaces:**
- Consumes: Task 1 (`config::ShellKind`, `RcState`, `ShellRows`, `config::ShellRcMarker`). `platform/` sits below
  `window/` (1b R-b), so the editor includes only `config/ShellKind.hpp`.
- Produces (`platform/ShellRcEditor.hpp`, namespace `contour::platform`):
  - `enum class ShellRcError : uint8_t { NoHome = 0, Unreadable, Unwritable, NotAFile };`
    `[[nodiscard]] constexpr std::string_view describe(ShellRcError error) noexcept;` ("the home directory is not
    known", "the file could not be read", "the file could not be written", "it is not a regular file").
  - `struct RcInspection { std::filesystem::path file {}; config::RcState state = config::RcState::Unknown; bool operator==(RcInspection const&) const = default; };`
  - `enum class RcInstall : uint8_t { Appended = 0, AlreadyInstalled };`
  - `class IShellRcEditor { public: virtual ~IShellRcEditor() = default; [[nodiscard]] virtual std::expected<RcInspection, ShellRcError> inspect(config::ShellKind shell) const = 0; [[nodiscard]] virtual std::expected<RcInstall, ShellRcError> install(config::ShellKind shell, std::string_view line) = 0; };`
  - `class NullShellRcEditor final: public IShellRcEditor` (inline): `inspect` → `RcInspection {}` (`Unknown`, so no card
    is offered); `install` → `std::unexpected(ShellRcError::Unwritable)`.
  - `class FileShellRcEditor final: public IShellRcEditor { public: FileShellRcEditor(core::platform::FileSystem const& fileSystem, core::Environment const& environment, config::HostPlatform host); … };`
    — resolves the file per R-e from `HOME`, `ZDOTDIR`, `XDG_CONFIG_HOME` (`environment.get(name)`), inspects and
    installs per R-f.
- Produces (`config/GuiStateStore.hpp`): `GuiState` gains `std::vector<ShellKind> dismissedShellIntegrations {};`,
  stored as `dismissed_shell_integration: [zsh]` (the rows' names); an unknown token is skipped, as for
  `dismissed_new_defaults`.

- [ ] **Step 1: Write the failing tests.**
  - `ShellRcEditor_test.cpp` (`[shellrc]`), over `core::platform::InMemoryFileSystem` (the namespace its header
    declares) and a test `core::Environment` with `HOME=/home/me`:
    - an absent `~/.zshrc` → `inspect(Zsh)` is `{ "/home/me/.zshrc", Absent }`; `install(Zsh, line)` → `Appended`, and the
      file is exactly `line + "\n"`;
    - "no trailing newline": a file `"export A=1"` becomes `"export A=1\n" + line + "\n"`;
    - "a second install writes nothing": install twice → `Appended`, then `AlreadyInstalled`, the bytes unchanged;
    - "a manual install counts": a file holding `source ~/.config/contour/shell-integration.zsh` → `Installed`, and
      `install` writes nothing; for fish an existing `~/.config/fish/conf.d/contour.fish` → `Installed`;
    - `ZDOTDIR=/home/me/.config/zsh` → the zsh file is `/home/me/.config/zsh/.zshrc`; fish creates
      `/home/me/.config/fish/` before appending; `HostPlatform::MacOS` → bash's file is `/home/me/.bash_profile`,
      `Other` → `/home/me/.bashrc`;
    - no `HOME` → `NoHome`; a directory at the rc path → `NotAFile`;
    - "only ever appends" (Review Focus 5): a decorator `FileSystem` in the test that forwards to the in-memory one and
      counts calls sees `appendFile` and never `writeFile`, `rename` or `replaceFile` on the rc path;
    - `NullShellRcEditor`: `inspect` is `Unknown`, `install` is `Unwritable`.
  - `GuiStateStore_test.cpp` (`[guistate]`): save then load round-trips `{ Zsh, Fish }`; a file holding
    `dismissed_shell_integration: [zsh, csh]` loads `{ Zsh }`; `dismissed_new_defaults` survives a save of the new key.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test`

  Expected: compile error, `platform/ShellRcEditor.hpp` not found.
- [ ] **Step 3: Implement** the editor and the state key.
- [ ] **Step 4: Run them and see them pass.**

  Run: `out/appleclang-debug/src/contour/contour_test "[shellrc],[guistate],[shellintegration]"`

  Expected: PASS.
- [ ] **Step 5: Lint, format and commit** (`editorconfig-checker src/contour/platform/CMakeLists.txt`):
  `git commit -m "contour: an rc-file editor that only appends, once, and a remembered Not Now per shell" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"`.

---

### Task 3: The session offers, and the press installs

> **Depends on Step 2:** `vtpty::Pty::foregroundProcessName()` (Step 2 Task 5); the pane's session end (`_end`,
> Task 10), which hides the card. **Depends on Step 3c:** `NoticeKind`, `NoticeRows`, `TerminalSessionManager::postNotice`
> (3c Tasks 5–6). **Depends on Step 1a:** `GuiStateStore` on the app.

**Files:**
- Modify: `src/contour/session/TerminalSession.hpp`, `.cpp` — the probe, fed where `sendKeyEvent` writes an
  unmodified Return to the PTY; the card's properties and invokables.
- Modify: `src/contour/session/TerminalSessionManager.hpp`, `.cpp` — the offer, the slot, install and dismissal.
- Modify: `src/contour/ContourGuiApp.hpp`, `.cpp` — a constructor parameter
  `std::unique_ptr<platform::IShellRcEditor> shellRcEditor = nullptr` (null: a `FileShellRcEditor` over `fileSystem()`,
  the environment and the host platform), `[[nodiscard]] platform::IShellRcEditor& shellRcEditor() noexcept;`,
  `[[nodiscard]] std::filesystem::path contourBinaryPath() const;` (`QCoreApplication::applicationFilePath()`).
- Modify: `src/contour/window/NoticeQueue.hpp` — rows `ShellIntegrationAdded` (Toast, —, 4000 ms, None) and
  `ShellIntegrationFailed` (Toast, `!`, 6000 ms, None), with `shellIntegrationAddedText(std::string_view rc, std::string_view shell)`
  and `shellIntegrationFailedText(std::string_view rc, platform::ShellRcError error)`.
- Create: `src/contour/test/RecordingShellRcEditor.hpp` — scripted `RcInspection` per shell, a scripted install
  result, and the recorded `(shell, line)` installs.
- Modify: `src/contour/test/GuiTestFixtures.hpp` — `TestApp` passes a `RecordingShellRcEditor` unless a test hands
  its own, and exposes it as `shellRcEditor()`.
- Test: `src/contour/session/TerminalSession_test.cpp`, `src/contour/session/TerminalSessionManager_test.cpp`,
  `src/contour/window/NoticeQueue_test.cpp`.

**Interfaces:**
- Consumes: Tasks 1 and 2.
- Produces (`TerminalSession`, all `NOTIFY shellIntegrationCardChanged`):
  `Q_PROPERTY(int shellIntegrationCard READ shellIntegrationCard …)` (a `CardStage`), `shellIntegrationTitle`,
  `shellIntegrationBody`, `shellIntegrationPrimary`, `shellIntegrationSecondary`, `shellIntegrationError` (`QString`s);
  `Q_INVOKABLE void acceptShellIntegrationCard();` (Offer → Confirm; Confirm → install);
  `Q_INVOKABLE void declineShellIntegrationCard();` (Offer → remembered Not Now; Confirm → hidden for the run);
  `void showShellIntegrationCard(config::ShellKind shell, std::filesystem::path rcFile, std::string line);` (manager only).
  The probe's `marks` is `Seen` when `terminal().commandBlocks().size() > 0`, read under the terminal lock.
- Produces (`TerminalSessionManager`):
  `void offerShellIntegration(TerminalSession& session, config::ShellKind shell);` — builds `OfferInputs` (locality
  from `attachedHostLabel()`, the dismissal from the GUI state, `inspect(shell)`, the run's `CardSlot`), and on `Card`
  takes the slot and calls `showShellIntegrationCard` with `shellRcLine(shell, contourBinaryPath())`;
  `[[nodiscard]] std::expected<platform::RcInstall, platform::ShellRcError> installShellIntegration(config::ShellKind shell);`
  (posts the toasts of R-j to the session's window);
  `void dismissShellIntegration(config::ShellKind shell);` (appends to `GuiState::dismissedShellIntegrations`, saved;
  a save error is logged, the card still hides).

- [ ] **Step 1: Write the failing tests.**
  - `TerminalSession_test.cpp` (`[contour][session][shellintegration]`), a `MockPty` whose foreground process is
    `"zsh"`, the app's binary path set to `/opt/contour/bin/contour`:
    - "three commands without a prompt mark offer the card": three Return presses → `shellIntegrationCard ==
      int(CardStage::Offer)`, title `"Turn on command blocks"`, body names `zsh`; the recording editor saw no install.
    - "a prompt mark ends the probe": `\033]133;A\a` written before the third Return → the card stays `Hidden`.
    - "Returns in vim do not count": `\033[?1049h`, three Returns → `Hidden`.
    - (Review Focus 3) "ssh in the foreground offers nothing": foreground `"ssh"` → `Hidden`.
    - "Install shows the line first and writes only on Add Line": accept → `Confirm`, body equals
      `shellRcLine(Zsh, "/opt/contour/bin/contour")`, title `"Add this line to ~/.zshrc?"`, no install yet; accept →
      one install with exactly that line, `Hidden`, and the window's toast is
      `"Added shell integration to ~/.zshrc. New zsh sessions show command blocks."`.
    - "a failed write keeps the card": install scripted `Unwritable` → still `Confirm`, `shellIntegrationError ==
      "~/.zshrc was not changed: the file could not be written."`, the toast posted.
    - "Not Now remembers the shell": decline on Offer → the GUI state holds `Zsh`; a new session's three commands →
      `Hidden`. Decline on Confirm → `Hidden`, nothing remembered.
    - "an integrated rc offers nothing": `inspect(Zsh)` scripted `Installed` → `Hidden`.
    - "an ended pane hides the card": the card shows, then the PTY closes early (Step 2's end) → `Hidden`.
  - `TerminalSessionManager_test.cpp` (`[contour][session][manager][shellintegration]`):
    - "one card per run": two sessions cross the threshold → only the first shows it.
    - (Review Focus 3) "a remote-hosted window offers nothing": an app built with host label `"starfive"` → `Hidden`.
    - (Review Focus 1) "TestApp never reaches a file editor": `dynamic_cast<test::RecordingShellRcEditor*>(&app.shellRcEditor()) != nullptr`.
  - `NoticeQueue_test.cpp` (`[notices]`): the two rows are in enumerator order; the texts above.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test contour_gui_test`

  Expected: compile errors on `shellIntegrationCard` and `RecordingShellRcEditor.hpp`.
- [ ] **Step 3: Implement** as stated.
- [ ] **Step 4: Run them and see them pass.**

```bash
out/appleclang-debug/src/contour/contour_test "[notices],[shellintegration]"
out/appleclang-debug/src/contour/contour_gui_test "[contour][session][shellintegration],[contour][session][manager],[contour][session]"
```

  Expected: PASS.
- [ ] **Step 5: Lint, format and commit** the files above by name:
  `git commit -m "contour: offer shell integration after three commands without prompt marks; install on a second press" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"`.

---

### Task 4: `ShellIntegrationCard.qml`, its tokens and its snapshots

> **Depends on Step 2:** `PaneCard.qml`'s bottom chrome, `ExitedFooter.qml`, `size.footerButton`, `fill(3)`
> (Step 2 Tasks 3, 9, 11). **Depends on Step 1c:** `chipOpenGlyph`/`chipCloseGlyph`.

**Files:**
- Create: `src/contour/qml/ShellIntegrationCard.qml`; add it to `_qml_files`.
- Modify: `src/contour/qml/PaneCard.qml` — the card above the footer slot; the terminal's bottom anchors to the
  card's top while it shows; hidden while `session.sessionEnd !== 0`.
- Modify: `src/contour/config/ChromeTokenGroups.hpp`, `UiStyle.hpp`, `src/contour/window/ChromeTokenObjects.{hpp,cpp}`,
  `src/contour/window/UiStyleProvider.{hpp,cpp}`, `src/contour/test/QmlChromeStyle.hpp` — R-l's four tokens on all
  four rows.
- Modify: `src/contour/test/QmlMocks.hpp` (`MockSession`: the six card properties, the two invokables counting calls).
- Modify: `src/contour/test/ChromeSnapshotPlan.hpp`, `ChromeSnapshotScene.hpp` — rows `ShellIntegrationOffer`
  (`"shell-integration-offer"`) and `ShellIntegrationConfirm` (`"shell-integration-confirm"`), single leaf, a
  `SnapshotShellCard` field (`None = 0, Offer, Confirm`) on every row.
- Test: `src/contour/test/QmlComponents_test.cpp`, `Accessibility_qml_test.cpp`, `ChromeSnapshotPlan_test.cpp`,
  `ChromeSnapshot_test.cpp`, `src/contour/config/ChromeTokenGroups_test.cpp`.

**Interfaces:**
- Consumes: Task 3's properties.
- Produces (QML): `ShellIntegrationCard.qml`, `objectName: "shellIntegrationCard"`, `required property var session`,
  visible while `session.shellIntegrationCard !== 0`; `shellIntegrationTitle`, `shellIntegrationBody` (in Confirm, set in
  the session's own terminal font family, `session.fontFamily` — the pre-flight confirms the property's name or adds
  it — and a read-only `TextEdit` with `selectByMouse`, so the line can be copied),
  `shellIntegrationError` (`statusColor(Danger)`), `shellIntegrationPrimary` (accent, calls
  `session.acceptShellIntegrationCard()`), `shellIntegrationSecondary` (calls `declineShellIntegrationCard()`).
  `Accessible.role: Accessible.AlertMessage` named by the title; the buttons `Accessible.Button`. Escape on the card
  is Not Now / Cancel.

- [ ] **Step 1: Write the failing tests.**
  - `ChromeTokenGroups_test.cpp` (`[chrometokens]`): the four tokens per row as in R-l.
  - `QmlComponents_test.cpp` (`[contour][gui][qml][shellintegration]`): with the mock at `Offer`, the card is visible,
    `width == 330` on MacOS, radius 12, the terminal's bottom equals the card band's top; clicking Install counts one
    `acceptShellIntegrationCard()`, Not Now one `declineShellIntegrationCard()`; at `Confirm` the body text is the mock's
    line and selectable; `sessionEnd = Exited` hides the card and shows `exitedFooter`; texts read at 4.5:1 over the
    card fill on contour-dark and contour-light. Under `ContourTui`: the card is three `cellHeight` rows and the buttons
    are bracketed.
  - `Accessibility_qml_test.cpp`: the card is an `AlertMessage` with two buttons.
  - `ChromeSnapshotPlan_test.cpp` (`[snapshot]`): the two rows' fields; every row names `shellCard`.
  - `ChromeSnapshot_test.cpp` (`[contour][gui][qml][snapshot]`): both scripts build on the MacOS and Terminal rows
    without a QML warning and find `shellIntegrationCard`.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test contour_gui_test && out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][shellintegration]"`

  Expected: FAIL, `shellIntegrationCard` not found.
- [ ] **Step 3: Implement** the tokens, the card, its place in `PaneCard.qml` and the snapshot rows.
- [ ] **Step 4: Run them and see them pass.**

```bash
out/appleclang-debug/src/contour/contour_test "[chrometokens],[snapshot]"
out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][shellintegration],[contour][gui][qml][paneend],[contour][gui][qml][a11y],[contour][gui][qml][snapshot]"
CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][shellintegration],[contour][gui][qml][snapshot]"
ctest --test-dir out/appleclang-debug -L lint --output-on-failure
```

  Expected: PASS; `ShellIntegrationCard.qml` is at 0 in both QML baselines.
- [ ] **Step 5: Lint, format and commit** the files above by name:
  `git commit -m "contour: the shell integration card, in the pane's bottom chrome" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"`.

---

### Task 5: Settings › Shell Integration › Install

> **Depends on Step 4b:** the Shell Integration section (`SettingsSection::ShellIntegration`), its page QML and the
> `settings-shell-integration` snapshot row (4b Tasks 4–6, 12). 4b R-m left these rows to 6.1.

**Files:**
- Modify: `src/contour/window/SettingsController.hpp`, `.cpp`; the Shell Integration page QML 4b creates (locate by
  `objectName: "settingsPageShellIntegration"`, pre-flight).
- Test: `src/contour/window/SettingsController_test.cpp`, `src/contour/test/SettingsPageQml_test.cpp`.

**Interfaces:**
- Consumes: Tasks 1–3 (`ShellRows`, the app's editor, `TerminalSessionManager::installShellIntegration`).
- Produces (`SettingsController`): `Q_PROPERTY(QVariantList shellIntegrationRows READ shellIntegrationRows NOTIFY shellIntegrationRowsChanged)`
  — one map per `ShellRows` row: `shell` (int), `name`, `status` ("Installed in ~/.zshrc" · "Not installed" ·
  "Unknown: {describe(error)}"), `canInstall` (QML carve-out bool, true for `Absent`);
  `Q_INVOKABLE QString shellIntegrationLine(int shell) const;`, `Q_INVOKABLE QString shellIntegrationRcPath(int shell) const;`,
  `Q_INVOKABLE void installShellIntegration(int shell);` (refreshes the rows). The page shows a group "Install" first,
  each row's Install… opening a `KitDialog` with the Confirm copy (title, the line in the terminal font, "Add Line" ·
  "Cancel"); Add Line calls `installShellIntegration`.

- [ ] **Step 1: Write the failing tests.**
  - `SettingsController_test.cpp` (`[settings][shellintegration]`): with the recording editor scripting zsh
    `Installed` at `~/.zshrc`, bash `Absent`, fish an `Unreadable` error: three rows in `ShellRows` order with those
    statuses and `canInstall` only for bash; `installShellIntegration(int(Bash))` records one install of
    `shellRcLine(Bash, …)` and the bash row becomes "Installed in ~/.bashrc" once the editor reports it.
  - `SettingsPageQml_test.cpp` (`[settings][qml][shellintegration]`): the Install group is the section's first; the
    bash row's Install… opens a dialog whose text is the line; Cancel installs nothing; Add Line counts one call.
    The same under `ContourTui`.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_gui_test && out/appleclang-debug/src/contour/contour_gui_test "[settings][shellintegration]"`

  Expected: FAIL, no `shellIntegrationRows`.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run them and see them pass.**

```bash
out/appleclang-debug/src/contour/contour_gui_test "[settings][shellintegration],[settings][qml][shellintegration],[settings]"
CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[settings][qml][shellintegration]"
ctest --test-dir out/appleclang-debug -L lint --output-on-failure
```

  Expected: PASS.
- [ ] **Step 5: Lint, format and commit** the files above by name:
  `git commit -m "contour: Settings shows and installs shell integration per shell" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"`.

---

### Task 6: Finish

- [ ] **Step 1:** Run `/simplify` on the step's diff (`git diff <the plan commit>..HEAD -- src scripts`), apply its
  findings, re-run the filters of every task it touched, and commit once with the Signed-off-by line, staging only
  the files it changed.
- [ ] **Step 2: Every filter once more.**

```bash
out/appleclang-debug/src/contour/contour_test "[shellintegration],[shellrc],[guistate],[notices],[chrometokens],[snapshot]"
out/appleclang-debug/src/contour/contour_gui_test "[contour][session][shellintegration],[contour][session][manager],[settings],[contour][gui][qml]"
CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][shellintegration],[settings][qml][shellintegration],[contour][gui][qml][snapshot]"
ctest --test-dir out/appleclang-debug -L lint --output-on-failure
```

  Expected: PASS.
- [ ] **Step 3: Snapshots, including the Terminal row**, on this Mac's display (windows flash; CI runs them under
  weston):

```bash
export CONTOUR_SNAPSHOT_DIR="$PWD/out/chrome-snapshots"; rm -rf "$CONTOUR_SNAPSHOT_DIR"
CONTOUR_TEST_DISPLAY=1 out/appleclang-debug/src/contour/contour_gui_test "[display][snapshot]"
CONTOUR_TEST_DISPLAY=1 CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[display][snapshot]"
ls "$CONTOUR_SNAPSHOT_DIR"/*.png | wc -l
```

  Expected: `SnapshotScripts.size() × 4 rows × 2 editions × 2 appearances`, the size read from
  `ChromeSnapshotPlan.hpp`. Look at `shell-integration-offer.*` (the canvas card, bottom-centred, the grid above it),
  `shell-integration-confirm.*` and `settings-shell-integration.*`.
- [ ] **Step 4: Owner manual checks** (batched for Phase 6; list them in the ledger, do not wait). From
  `out/appleclang-debug/src/contour/contour.app` with a scratch `HOME` (`HOME=$(mktemp -d)`) and `zsh`: run three
  commands → the card; Install → the line and `~/.zshrc`; Add Line → the toast, the file holds the line once; a new
  tab → command blocks; press it again in Settings → "Installed in ~/.zshrc"; Not Now in a fresh `HOME` → no card in
  the next pane; `ui_style: terminal` → the cell card.
- [ ] **Step 5: Release notes** with `/add-release-note` in `metainfo.xml`, under the open `0.9.0` release:
  "Offers to install shell integration for bash, zsh and fish when a shell writes no prompt marks: the card shows the
  one line it would add to the shell's rc file, and adds it only when asked." and "Shows and installs shell
  integration per shell in Settings › Shell Integration."
- [ ] **Step 6:** Mark Step 6.1 done in the plans index's step table and *Last update*, and on the board with
  `/status-board update`. Copy the SDD ledger to `docs/superpowers/ledgers/2026-10-25-native-redesign-step-6-1.md`.
  Update the PR body with `/update-pr`, reporting:
  - **Performance:** one counter per session until the third command; one `foregroundProcessName()` read and one rc
    read per run; nothing on the render path.
  - **Risk:** low to medium. The step writes a user's dotfile, only after two presses and only by appending; a pane
    shrinks by the card's band once.
  - **Coverage:** the pure tables, the editor over an in-memory file system, the mock-PTY session cases, the QML
    property tests on the native and Terminal rows, two snapshot scripts.
- [ ] **Step 7:** Run `/code-review` on the whole step and address every finding.

---

## Owner decisions

The run proceeds on each recommended default.

1. **The guard and the path (R-d).** *Default: guard on `TERM_PROGRAM = contour`, the running binary's absolute
   path.* The alternative is an unguarded `contour` on `PATH`, which breaks when the `.app` is not on `PATH`.
2. **The card reserves the bottom rows (R-i).** *Default: reserve (§1.5).* The canvas floats it over blank rows,
   which needs a blank-row test and a fallback.
3. **Not Now is per shell and permanent (R-h).** *Default: per shell, in `gui-state.yml`; Settings still installs.*
4. **Three shells (R-b).** *Default: bash, zsh, fish.* tcsh and pwsh have scripts and could be rows later.
5. **Both editions, no config key (R-k).** *Default: yes.* The alternative is a `shell_integration.offer` key in the
   edition table.
6. **bash on macOS uses `~/.bash_profile` (R-e).** *Default: yes.*
7. **The running shell is left alone after Install (R-j).** *Default: yes; new shells pick it up.*

## Spec gaps found

- "3 prompts without OSC 133" cannot be counted (prompts are what OSC 133 marks); read as three commands (R-a).
- Where the shell comes from, which rc file each shell uses on which OS, and what "the one-line source" says are
  unspecified (R-b, R-d, R-e).
- The canvas floats the card over the bottom rows, which §1.5 forbids when they hold output (R-i).
- Step 4b R-m hands Settings' install rows to 6.1; §3.16 does not list them (Task 5).
- fish 4.0 and later mark A, C and D themselves, so a fish 4 user is never offered the card (correct, not a gap in
  behaviour, but worth knowing when testing).
