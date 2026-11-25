# Orchestrator Defaults and Vim Keys Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Agents start in Claude Code's `auto` permission mode through a new built-in `auto` profile that is the
default. `j k h l gg G Ctrl+D Ctrl+U q i` work in every list, dialog and menu whenever nothing is being typed; the
palette, which is always typing its query, chooses with `↑ ↓` and `Ctrl+J`/`Ctrl+K`. The dialog's and the `n` box's
text fields switch between typing and navigating. A `keys` map rebinds the navigation keys, and `work.folded` /
`work.fold_empty` set the Work view's folds.

**Architecture:** Bottom-up. `core/Permissions` gains the `auto` mode row and the `auto` built-in profile. A new
`core/NavKeys` holds the navigation vocabulary. It has one row per action with its vim and plain default keys, a
resolved `NavKeys` binding table, and the clash rules. `core/DashboardKeys` gains `commandKeys()`, the keys the
views bind to commands. The configuration reads `keys` and `work` through these. The view gets a single resolver,
`view/NavInput`. It names a key, holds a pending `g` of `gg`, and turns a key into a `NavAction` for the surface
that has the keys. Every surface consults the resolver's result instead of matching arrow keys itself: the Fleet,
the Focus, the Work list, the start dialog, the `n` box, the palette, the row menu, the timeline, the filters and
the message box. The start dialog and the `n` box gain a `FieldMode` (typing or navigating). Work-view folds become explicit
user choices (`SetFold`), so a configurable default can never flip them.

**Tech Stack:** C++23, Catch2 (`orchestrator_test`), glaze (orchestrator JSON), core-cpp's `core::tui`
(headless), the installed `claude` CLI for one verification.

**Spec:** `docs/superpowers/specs/2026-11-25-orchestrator-defaults-and-vim-keys-design.md` (approved, binding).
This plan implements its **Goals 1–3**. **Goal 4 (large repositories) is already done**, in commits
`6aa7ba8ef`..`ecf663052` (`command-drop-lines`, the index check, and its docs), and is not planned here.

## Global Constraints

- **The user's uncommitted work.** `docs/extensions.md` and `cmake/ContourWasiCxx.cmake` carry the user's own
  uncommitted hunks. For the whole plan, never run `git add -A`, `git add .`, `git add -u`, `git commit -a`, or a
  pathspec commit (`git commit -- <path>`). Stage named paths only, and run `git diff --cached --name-only` before
  every commit: it must list exactly the task's files. Never run `git stash`, `git checkout -- <file>`,
  `git restore`, `git reset --hard`, a formatter or `sed -i` on either file. This plan does not edit
  `cmake/ContourWasiCxx.cmake`.
- **`docs/extensions.md` is never edited by an implementer.** Make every change to it via the WIP-preserving
  procedure the controller runs. The implementer writes the new text into
  `D:\repo\contour\.superpowers\sdd\2026-11-25-orchestrator-defaults-and-vim-keys\extensions-text.md` instead (git
  ignores that directory). Each task appends one section `## Task N`. The section gives each table row of
  "Configuring the orchestrator" it replaces or adds, in full, with the row it follows.
- **The user's own `contour.yml` is outside the repository and is not edited.** The controller names `auto` in it
  after the plan ships (spec, "Left to the builder").
- **Never edit `vendor/core-cpp`.**
- **Builds:** one at a time, from the repository root, 10-minute timeout, rerun on timeout. Git Bash:
  `CMAKE_BUILD_PARALLEL_LEVEL=2 cmake --build --preset clangcl-release -j 2 --target orchestrator_test`. Do not
  reconfigure `out/build/clangcl-release` and do not delete its `_deps`. The test binary is
  `out/build/clangcl-release/bin/orchestrator_test.exe`.
- **Goldens.** `ORCHESTRATOR_UPDATE_GOLDENS=1 out/build/clangcl-release/bin/orchestrator_test.exe "golden screens"`
  writes `extensions/orchestrator/view/testdata/<name>.screen`. Rerun without the variable: it must pass. Read every
  new or changed `.screen` before committing. A changed screen may differ only in what the task changes. This plan
  declares no command, so `contract/manifest-v2/valid/orchestrator.manifest` and `orchestrator.consent.txt` must
  not change. `ctest --test-dir out/build/clangcl-release -R orchestrator` → all pass after every task.
- **AGENT.md:**
  - C++23, range-for only, `[[nodiscard]]`, and Doxygen on new public API.
  - `enum class : uint8_t` over `bool`, with the zero enumerator the negative or default case. `vim` and
    `fold_empty` are `bool` only in `ConfigJson.hpp` (the wire boundary) and become `VimKeys` / `EmptyGroups`
    inside.
  - Data-driven tables: one row per navigation action, command key, group spelling and problem text.
  - Constructor-complete objects and `std::expected` for fallible API.
  - No `NOLINT` beyond the existing `glz::meta` `value` spelling. Zero warnings.
  - Run `clang-format -i` on every C++ file touched. Headers are `.hpp` with `#pragma once`.
- **wasm32-wasi portability.** After every task that touches `extensions/orchestrator/{core,view,guest}`,
  `python scripts/check-guest-portability.py` and `python scripts/check-include-budget.py` exit 0. That means no
  `<regex>`, no exceptions, no `at(`, and glaze only in `.cpp`.
- **Every configuration valid today stays valid.** `keys` and `work` are optional. `dashboardKeys()` is unchanged,
  so no task key becomes reserved: a task keyed `q`, `g`, `G`, `h`, `l` or `i` still loads. The top-level
  `permissions` key keeps naming the default profile.
- **Keyboard parity:**
  - Arrows, `Tab`/`Shift+Tab`, `Home`/`End`, `PgUp`/`PgDn` and the mouse keep working as today.
  - The `n` box opens with the keys in its text, so typing and then `⏎` starts at once.
  - No letter types into a field that is not being typed, and no letter acts while one is.
- **No real data** in committed files. Use `example.atlassian.net`, `group/project`, `PRJ-<n>`, `user`, and the
  repositories `contour`/`other`.
- Commit messages end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- **Subagent models (user's rule):** Haiku for Tasks 1 and 3, which are fully specified. Opus for Tasks 2, 4, 5,
  6 and 7, which need clash rules, engine and guest wiring, view routing and goldens. Every review runs on Opus. A
  Haiku task that comes back blocked, or reaches a 4th fix round, is re-dispatched on Opus.

## Decisions (the builder's, per the spec's "Left to the builder", and where the code argued)

1. **The default profile stays `permissions`, not `default_permissions`.** The top-level `permissions: <profile>`
   key already names the profile a task without its own runs with (`Config::defaultProfile()`, docs row
   `permissions`). A second spelling of the same setting would be two keys for one thing. The spec's
   `default_permissions` is therefore this existing key, and its default becomes `auto`.
2. **`auto` extends `standard`.** The built-in `auto` profile is `standard` with mode `auto`. Its 19 allowed and 4
   denied rules stay, so `sudo`, `rm -rf`, a forced push and `git reset --hard` are still denied whatever Claude
   Code's `auto` decides. `DefaultProfileName` becomes `"auto"`. A new `StandardProfileName = "standard"` names the
   profile `trusted` and `auto` extend. `builtinProfiles()` lists `auto`, `standard`, `trusted`, `ask`.
3. **Claude Code's spelling.** `claude --help` (2.1.332) lists `--permission-mode` choices `acceptEdits`, `auto`,
   `bypassPermissions`, `manual`, `dontAsk` and `plan`. The orchestrator passes the mode through `--settings`
   (`permissions.defaultMode`), not `--permission-mode`. The installed binary's settings enum reads
   `"acceptEdits","auto","bypassPermissions","default","dontAsk","plan"`, so `default` (the `ask` profile) and
   `auto` are both settings values. Task 1 confirms this by running the installed `claude` before anything is
   committed.
4. **One navigation table, in the core.** `navActionRows()` has one row per action: its config name, the context it
   acts in, its vim keys and its plain keys. A resolved `NavKeys` maps a key name to an action per context. The
   configuration validates against it, and every view consults it. The view's `KeyTable` and `WorkKeyTable` lose
   their `up k down j home end left right` rows. The nav table now supplies those keys and maps them to the same
   `KeyCommand`s (Decision 9).
5. **Two contexts.** `LeaveField` acts in `Typing`. Every other action acts in `Navigating`. A key may be bound in
   both contexts (`esc` is `leave_field` and `close`), never twice in one. A `leave_field` key must not be text: a
   printable key or a sequence is refused (`TypesText`), since it would stop the user typing that letter
   (anti-goal).
6. **Default key order is plain first, then vim:** `move_down: [down, j]`. The action bar and every hint show an
   action's first key, so with the defaults they read as today (`↑↓`, `←→`, `esc`, `⏎`). The `enter_field` hint
   shows its first key that is not `enter` (`i`). `vim: false` drops the vim keys, leaving arrows, `Home`/`End`,
   `Esc` and `Enter`. A configured action list replaces that action's defaults whole.
7. **`gg` is the one kind of sequence: two printable characters.** The resolver keeps the first in
   `Interaction::pendingKey`. The next key either completes the sequence or drops the pending key and is handled on
   its own; a mouse event drops it too. There is no timeout. A sequence's first character may not be a key bound
   alone in the same context or a command key, and the reverse is also refused. Key spellings:
   - one printable ASCII character other than space;
   - `^` and a lower-case letter other than `h`, `i`, `j` or `m`, which terminals send as Backspace, Tab, LF and CR;
   - two printable characters not starting with `^`;
   - one of `up down left right home end pgup pgdn enter esc tab del`.

   An integer `0`–`9` in YAML is read as that digit, as a task `key` is.
8. **Clashes are checked against `commandKeys()`, not `dashboardKeys()`.** `dashboardKeys()` reserves task keys
   and includes `j`, `k` and the arrows, which are navigation. It omits `w`, `s`, `o`, `r` and the digits, which
   the views bind to commands (work-board Decision 6). `commandKeys()` lists every key a view binds to a command,
   per context, with what it does. A binding may take a key in its own action's default row, which is how
   `enter_field` keeps `enter` and `close` keeps `esc`. A view test keeps `keyRows()`/`workKeyRows()` and
   `commandKeys()` in step. A refused binding is a `ConfigError` at the configured entry (`keys.close[0]`). Its
   subject names the other holder, as in `keys.close[0]: is already bound: j, also keys.move_down`. When both
   holders are configured, the error is reported at the later one in `navActionRows()` order. `close: []` and
   `leave_field: []` are refused as `Empty`, since a dialog would then have no key that closes it.
9. **What each action does where.** Lists consult only the movement actions:

   | Action | Fleet | Focus (no message box) | Work list | Start dialog / `n` box | Row menu (the palette: Decision 15) |
   | --- | --- | --- | --- | --- | --- |
   | `move_up` / `move_down` | `Up`/`Down` | scroll a line | `Up`/`Down` | previous / next setting, stopping at the ends | highlight up / down |
   | `first` / `last` | `Home`/`End` | top / end | `Home`/`End` | first / last setting | first / last entry |
   | `half_page_up` / `_down` | selection by `max(1, fleetRows / 2)` | scroll by `max(1, rows / 2)` | selection by `max(1, pageRows / 2)` | nothing | nothing |
   | `previous_value` / `next_value` | `PreviousPage`/`NextPage` | same | the chip with the keys: the sort chip toggles the sort, else the repository chip steps | step the setting | nothing |
   | `enter_field` | nothing (`⏎` stays the row's command) | nothing | nothing | `⏎` on the text line: type there; elsewhere `⏎` confirms; any other `enter_field` key: to the text, typing | — |
   | `close` | closes the timeline | same | nothing | close | close |
   | `leave_field` (typing) | — | the message box: back to the Fleet, draft kept | the filter: stop typing, filter kept | to navigating; the `n` box moves to `Repo` | — |

10. **The filter.** `Esc` now leaves the filter and keeps it, as `⏎` does. Today it drops it. A second `Esc` (the
    lists' existing `Back`) clears it. `/` enters the filter again with its text kept. `i` is not bound in lists,
    because a list's `⏎` is its row's action and a list has no selected text field. The spec's "`i` or `Enter`
    enters it again" applies to the boxes.
11. **Alt.** A key with Alt is how `Esc` reads when the next key follows it in one read. While a field is typed,
    it is `leave_field` and the key itself is dropped. While navigating, it closes the box as today.
12. **Folds are explicit choices.** Today a group's fold key held in the store means "toggled from the group's
    fixed default". With a configurable default, a toggle would silently flip when `work.folded` changes. A new
    engine input `SetFold { group, FoldChoice }` stores `<foldKey>#open` or `<foldKey>#folded` in the same
    `FoldsKey` array, after removing the group's other keys. There is no store-format change: it is still a JSON
    array of bounded keys. A group's state is decided in this order:
    1. its `#open` / `#folded` choice;
    2. a legacy bare key (stored before this plan), read against the built-in `FoldDefault`;
    3. `work.folded`, or the built-in default without a configuration.

    The sort keeps its toggle key.
13. **`fold_empty: true` is today's behaviour**: a group with no item shown loses its heading (`workLines`).
    `false` shows all five headings in the grouped sort, an empty group as `▾ No MR yet  0`. Group spellings are
    `needs_you`, `ready_to_merge`, `in_review`, `no_mr_yet` and `everything_else`.
14. **`WorkGroup` moves to `core/WorkGroups.hpp`.** `WorkItems.hpp` includes `Config.hpp`, so `Config` cannot
    include `WorkItems.hpp`. The group enum, `FoldDefault`, `WorkGroupRow` (which gains `configName`),
    `WorkGroupRows`, `SortByUpdateKey` and the new `EmptyGroups`/`WorkSettings` move to a header both include.
15. **The palette has no navigating mode (controller's ruling).** It opens typing its query and stays so: every
    printable key, `j` and `k` included, types into the query. `↑`/`↓` and `Ctrl+J`/`Ctrl+K` move the highlight
    (wrapping, as `↑`/`↓` do today), `⏎` runs, and `Esc` (or a key with Alt) closes it at once, keeping the query as
    today. The nav table is not consulted in the palette; `Ctrl+J`/`Ctrl+K` are fixed palette keys (`^k` is already
    in `commandKeys()` in context Typing; Ctrl+J is no valid binding spelling, Decision 7). Ctrl+J is LF: check how
    `core::tui` decodes 0x0A, and if it arrives as `KeyCode::Enter`, tell it from CR by its codepoint, so that it
    moves the highlight instead of running the command.

## Review Focus

1. **Claude Code cannot run the agent in `auto`**, for example because the account or model does not offer it.
   The orchestrator still passes the profile, Claude Code decides (spec Non-goals), and the profile's deny rules
   still hold. Task 1 pins that `settingsJsonOf(auto)` carries `standard`'s four deny rules. The README says how to
   choose `standard`.
2. **A configuration with a task keyed `q`, `g`, `G`, `h`, `l` or `i`** loads unchanged: Task 2 test.
3. **A vim letter pressed while typing** in the `n` box's text, the note, the palette's query, a filter or the
   message box types that letter and acts on nothing (anti-goal). Task 6 and Task 7 type `jkhlqgGi` into each.
4. **`g` followed by another key:** the other key acts as it would alone, and nothing jumps. Task 5 test.
5. **A fold the user made survives `work.folded` changing, and a fold stored before this plan still shows as
   left.** Task 4 test.

---

### Task 1: `auto` mode and the built-in `auto` profile, the default — Haiku

**Files:**
- Modify: `extensions/orchestrator/core/Permissions.hpp`, `extensions/orchestrator/core/Permissions.cpp`
- Modify: `extensions/orchestrator/core/Config.hpp` (the `NotPermissionMode` comment only)
- Modify: `extensions/orchestrator/README.md` ("Permissions", the `Perms` bullet of "Starting an agent")
- Modify: `extensions/orchestrator/view/testdata/start-dialog-*.screen` (regenerated, if they show the profile)
- Create: `.superpowers/sdd/2026-11-25-orchestrator-defaults-and-vim-keys/extensions-text.md` (ignored; `## Task 1`)
- Test: `extensions/orchestrator/core/Permissions_test.cpp`, `extensions/orchestrator/core/Config_test.cpp`,
  `extensions/orchestrator/core/Engine_test.cpp`

**Interfaces:**
- Produces:

```cpp
// Permissions.hpp
enum class PermissionMode : uint8_t { Default = 0, AcceptEdits, Plan, Auto /* Claude Code decides what needs approval */ };
// PermissionModeNames gains, last:
PermissionModeName { .word = "auto", .mode = PermissionMode::Auto, .effect = "Claude Code decides what needs approval" },
/// The profile a task without its own runs with, unless `permissions` names another.
inline constexpr std::string_view DefaultProfileName = "auto";
/// The built-in profile `trusted` and `auto` build on.
inline constexpr std::string_view StandardProfileName = "standard";
// Permissions.cpp BuiltinProfiles, in this order:
//   { auto,     extends StandardProfileName, mode Auto,        allow {}, deny {} }
//   { standard, extends {},                  mode AcceptEdits, StandardAllow, StandardDeny }   (named by StandardProfileName)
//   { trusted,  extends StandardProfileName, mode nullopt,     TrustedAllow, {} }
//   { ask,      extends {},                  mode Default,     {}, {} }
```

- [ ] **Step 1: Confirm Claude Code takes `auto` from `--settings`.** Run this from the scratchpad directory `$S`,
  not the repository, so that no project settings apply:

```bash
cd "$S" && for mode in auto acceptEdits default; do
  claude -p "Reply with the single word ok." --settings "{\"permissions\":{\"defaultMode\":\"$mode\"}}" \
    --output-format stream-json --verbose --max-turns 1 | head -1 | grep -o '"permissionMode":"[A-Za-z]*"'
done
```

  Expected: three lines, `"permissionMode":"auto"`, `"permissionMode":"acceptEdits"` and
  `"permissionMode":"default"`. If the first line names another mode, or `claude` rejects the settings, stop and
  report **BLOCKED** with the output. Nothing is committed in that case.
- [ ] **Step 2: Write the failing tests.** `Permissions_test.cpp` (`[orchestrator][permissions]`):

```cpp
CHECK(permissionModeOf("auto") == PermissionMode::Auto);
CHECK_FALSE(permissionModeOf(RefusedPermissionMode).has_value());
// resolveProfiles({}): four profiles; "auto" has mode Auto and exactly standard's rules.
auto const& autoProfile = named(*profiles, "auto");
CHECK(autoProfile.mode == PermissionMode::Auto);
CHECK(autoProfile.allow == named(*profiles, "standard").allow);
CHECK(autoProfile.deny == named(*profiles, "standard").deny); // Review Focus 1: the denies hold under auto
CHECK(settingsJsonOf(autoProfile).starts_with(R"js({"permissions":{"defaultMode":"auto","allow":["Bash(git status:*)",)js"));
CHECK(permissionsText("auto", settingsJsonOf(autoProfile))
      == "auto · 19 allowed · 4 denied · Claude Code decides what needs approval");
// A configured `standard` (mode plan) is what `auto` extends: auto keeps mode Auto and gains its rules.
```

  Update the existing case "the settings Claude Code takes" so that it looks up `standard` by
  `StandardProfileName`; its `acceptEdits` prefix stays. `Config_test.cpp`: `defaulted->profiles().size() == 4`, and
  `defaulted->defaultProfile().name == "auto"`. `"permissions": "standard"` still parses, and `profileFor(...)` is
  `standard`. `permission_profiles: [{ "name": "careful", "mode": "auto" }]` parses with mode `Auto`, and
  `"mode": "bypassPermissions"` is still `BypassRefused`. The `NotPermissionMode` message lists `default,
  acceptEdits, plan, auto`. In `Engine_test.cpp`, rename the section "by default, standard" to "by default, auto",
  and its two `"standard"` literals become `DefaultProfileName`.
- [ ] **Step 3:** Build, then run `orchestrator_test.exe "[permissions]"` and `"[config]"`. Expected: fail.
- [ ] **Step 4: Implement** the enum value, the row, the two constants and the four built-in rows. Update the
  `NotPermissionMode` comment to "other than those PermissionModeNames spells".
- [ ] **Step 5:** Rebuild and run `orchestrator_test.exe` (all). Regenerate the goldens (Global Constraints). The
  only allowed screen change is a profile name or effect text (`standard` → `auto`). Then run
  `ctest --test-dir out/build/clangcl-release -R orchestrator` and the portability scripts, and `clang-format -i`.
- [ ] **Step 6: Docs.**
  - README "Permissions": the default is `auto`, Claude Code's auto mode on top of `standard`'s rules. Claude Code
    decides what needs approval, and the four denies still hold. `standard` (edits need no approval, the rest
    asks), `trusted` and `ask` stay selectable, per task (`permissions:` on the task), as the default
    (`permissions: standard` at the top level), and in the start dialog's Perms. Add: "If your Claude Code cannot
    run an agent in auto mode, it decides how the agent runs; set `permissions: standard` to keep today's
    behaviour."
  - The `perms` example becomes `perms  auto · 19 allowed · 4 denied`.
  - `## Task 1` in `extensions-text.md` gives the full replacement rows `permissions` (default `auto`, what each
    built-in does) and `permission_profiles` (`mode` is `default`, `acceptEdits`, `plan` or `auto`).
- [ ] **Step 7: Commit:** the files above, minus `extensions-text.md`. Message:
  `feat(orchestrator): agents run in Claude Code's auto mode by default`.

### Task 2: The navigation-key vocabulary and the `keys` configuration — Opus

**Files:**
- Create: `extensions/orchestrator/core/NavKeys.hpp`, `extensions/orchestrator/core/NavKeys.cpp`,
  `extensions/orchestrator/core/NavKeys_test.cpp`
- Modify: `extensions/orchestrator/core/DashboardKeys.hpp/.cpp` (`CommandKey`, `commandKeys()`)
- Modify: `extensions/orchestrator/core/ConfigJson.hpp` (`JsonKeys`), `extensions/orchestrator/core/Config.hpp/.cpp`
  (`JsonType::Bool`, `JsonType::Object`, `JsonType::KeyList`, `ObjectKind::Keys`, `KeysFields`, problems,
  `Listing::KeyNames`, `Config::keys()`)
- Modify: `extensions/orchestrator/CMakeLists.txt`, `extensions/orchestrator/README.md` ("Configuration")
- Append: `extensions-text.md` (`## Task 2`)
- Test: `extensions/orchestrator/core/Config_test.cpp`

**Interfaces:**
- Produces (namespace `orchestrator`):

```cpp
// NavKeys.hpp
/// What a navigation key does.
enum class NavAction : uint8_t { MoveUp = 0, MoveDown, First, Last, HalfPageUp, HalfPageDown,
                                 PreviousValue, NextValue, LeaveField, EnterField, Close };
inline constexpr std::size_t NavActionCount = 11;
/// Where a navigation key acts: where nothing is typed, or in a text field being typed.
enum class NavContext : uint8_t { Navigating = 0, Typing };
/// Whether the vim keys are among the defaults (`keys.vim`).
enum class VimKeys : uint8_t { Off = 0, On };
/// One action: how `keys` names it, where it acts, and its default keys.
struct NavActionRow { NavAction action; std::string_view configName; NavContext context;
                      std::span<std::string_view const> plainKeys; std::span<std::string_view const> vimKeys; };
/// @return One row per NavAction, in its order (a new action is a new row).
[[nodiscard]] std::span<NavActionRow const> navActionRows() noexcept;
/// The key names that are no character.
inline constexpr auto SpecialKeyNames = std::array { "up"sv, "down"sv, "left"sv, "right"sv, "home"sv, "end"sv,
                                                     "pgup"sv, "pgdn"sv, "enter"sv, "esc"sv, "tab"sv, "del"sv };
/// What a key spelling is (Decision 7).
enum class KeyShape : uint8_t { Invalid = 0, Printable, Ctrl, Sequence, Special };
[[nodiscard]] KeyShape keyShapeOf(std::string_view name) noexcept;

/// The navigation keys in force: each action's keys, and the action a key does per context.
class NavKeys
{
  public:
    /// @return Every row's plain keys, then its vim keys while @p vim is On.
    [[nodiscard]] static NavKeys defaults(VimKeys vim);
    /// @param bindings Each action's keys, indexed by NavAction, already checked (navKeysOf()).
    explicit NavKeys(std::array<std::vector<std::string>, NavActionCount> bindings);
    [[nodiscard]] std::span<std::string const> keysOf(NavAction action) const noexcept;
    /// @return The action @p key does in @p context; std::nullopt: none.
    [[nodiscard]] std::optional<NavAction> actionFor(std::string_view key, NavContext context) const noexcept;
    /// @return Whether @p key is the first character of a sequence bound in @p context.
    [[nodiscard]] bool startsSequence(std::string_view key, NavContext context) const noexcept;
    [[nodiscard]] bool operator==(NavKeys const&) const = default;
  private:
    std::array<std::vector<std::string>, NavActionCount> _bindings;
};

/// Why a `keys` entry was refused.
enum class KeyRefusal : uint8_t { NotAKey = 0, TypesText, Taken, Unbound };
/// The configured entry refused, and why.
struct KeyBindingProblem
{
    NavAction action = NavAction::MoveUp;
    std::optional<std::size_t> index = {}; ///< Into the action's configured list; none for Unbound.
    KeyRefusal refusal = KeyRefusal::NotAKey;
    std::string subject = {};              ///< For Taken: the key and its other holder (Decision 8 formats).
    [[nodiscard]] bool operator==(KeyBindingProblem const&) const = default;
};
/// @param vim Whether the vim keys are among the defaults.
/// @param configured Per action (NavAction order): its configured keys; std::nullopt keeps its defaults.
/// @return The keys in force, or the first entry refused (actions in row order, entries in list order).
[[nodiscard]] std::expected<NavKeys, KeyBindingProblem> navKeysOf(
    VimKeys vim, std::span<std::optional<std::vector<std::string>> const, NavActionCount> configured);

// DashboardKeys.hpp
/// One key a view binds to a command: no navigation binding may take it in its context.
struct CommandKey { std::string_view name; std::string_view does; NavContext context; };
/// @return Every key the views bind to a command, per context, in the order a problem line would name them.
[[nodiscard]] std::span<CommandKey const> commandKeys() noexcept;

// Config.hpp
// ConfigProblem gains, after UnknownRole:
NotAKeyName, ///< A `keys` entry that is no key spelling.
KeyTaken,    ///< A `keys` entry another binding, or a command, already holds.
TypesText,   ///< A `leave_field` key that would type.
// Config gains: [[nodiscard]] NavKeys const& keys() const noexcept;   (Values::keys = NavKeys::defaults(VimKeys::On))
```

  `navActionRows()`, as `configName: context, plain, vim`:
  - `move_up`: N, `up`, `k`
  - `move_down`: N, `down`, `j`
  - `first`: N, `home`, `gg`
  - `last`: N, `end`, `G`
  - `half_page_up`: N, —, `^u`
  - `half_page_down`: N, —, `^d`
  - `previous_value`: N, `left`, `h`
  - `next_value`: N, `right`, `l`
  - `leave_field`: Typing, `esc`, —
  - `enter_field`: N, `enter`, `i`
  - `close`: N, `esc`, `q`

  `commandKeys()`, as `name does` (N = Navigating, T = Typing):
  - N:
    - `enter` "start or open the selected row", `esc` "go back", `tab` "move between panes and fields"
    - `del` "dismiss a finished run", `pgup` "scroll a page up", `pgdn` "scroll a page down"
    - `/` "filter", `?` "open the palette", `^k` "open the palette", `^p` "show the prompt"
    - `b` "show the board", `t` "show the transcript", `d` "show the details", `e` "end the run",
      `o` "open the link", `r` "reveal the agent", `x` "check the worktree again", `z` "fold a group",
      `n` "start a new task", `w` "switch the view", `s` "sort", `m` "open the row menu"
    - `1`–`9` "answer with quick reply <d>"
  - T: `enter` "send or start", `tab` "move to the next field", `^k` "open the palette" (in the palette: choose the entry above), `^u` "clear the text",
    `^w` "drop a word".

  Subjects for `Taken`:
  - another action holds the key: `"<key>, also keys.<other>"`
  - a command holds it: `"<key>, the dashboard's key to <does>"`
  - the key is a sequence starting with another binding's key: `"<key>, which starts with keys.<other>'s <prefix>"`
    (`"…with the dashboard's key to <does>"` when the prefix is a command key)
  - the key is the first character of another binding's sequence: `"<key>, which starts keys.<other>'s <sequence>"`

  `Config` maps a problem to `ConfigError { .key = "keys.<configName>[<i>]", … }` for `NotAKey` → `NotAKeyName`,
  `TypesText` → `TypesText` and `Taken` → `KeyTaken` (with the subject), and to `{ "keys.<configName>", Empty }`
  for `Unbound`. Problem texts:
  - `NotAKeyName`: "is not a key: one printable character, ^ and a letter (not h, i, j or m), two characters such
    as gg, or one of", followed by `Listing::KeyNames` (`SpecialKeyNames`).
  - `KeyTaken`: "is already bound".
  - `TypesText`: "would type into a text field instead of leaving it; use esc, ^ and a letter, or another special
    key".
  `JsonKeys` holds `std::optional<bool> vim` and, per action in row order, `std::optional<std::vector<std::string>>`,
  with `glz::meta` snake_case names. `KeysFields` mirrors it (`vim` Bool, the rest KeyList), with its
  `matchesMirror` static_assert. A `Config_test` case checks that its names after `vim` equal `navActionRows()`'
  `configName`s: for each row, `"keys": { "<configName>": ["F"] }` (`["^c"]` for `leave_field`) parses, and `keysOf(row.action)` is that list. `JsonType::Object` is one object of `Field::object`'s kind. `KeyList` is an array whose
  elements are strings or integers 0–9, read as `JsonType::Key` reads a task key. `Bool` is `true`/`false`.

- [ ] **Step 1: Write the failing tests.** `NavKeys_test.cpp`, tag `[orchestrator][navkeys]`:

```cpp
TEST_CASE("NavKeys: the defaults, with and without vim", "[orchestrator][navkeys]")
{
    auto const vim = NavKeys::defaults(VimKeys::On);
    CHECK(std::ranges::equal(vim.keysOf(NavAction::MoveDown), std::array { "down"sv, "j"sv }));
    CHECK(std::ranges::equal(vim.keysOf(NavAction::First), std::array { "home"sv, "gg"sv }));
    CHECK(std::ranges::equal(vim.keysOf(NavAction::HalfPageDown), std::array { "^d"sv }));
    CHECK(vim.actionFor("j", NavContext::Navigating) == NavAction::MoveDown);
    CHECK_FALSE(vim.actionFor("j", NavContext::Typing).has_value());
    CHECK(vim.actionFor("esc", NavContext::Typing) == NavAction::LeaveField);
    CHECK(vim.actionFor("esc", NavContext::Navigating) == NavAction::Close);
    CHECK(vim.actionFor("gg", NavContext::Navigating) == NavAction::First);
    CHECK(vim.startsSequence("g", NavContext::Navigating));
    CHECK_FALSE(vim.startsSequence("j", NavContext::Navigating));
    auto const plain = NavKeys::defaults(VimKeys::Off);
    CHECK(std::ranges::equal(plain.keysOf(NavAction::MoveDown), std::array { "down"sv }));
    CHECK(plain.keysOf(NavAction::HalfPageDown).empty());
    CHECK_FALSE(plain.actionFor("q", NavContext::Navigating).has_value());
    CHECK(plain.actionFor("esc", NavContext::Navigating) == NavAction::Close);
}
TEST_CASE("keyShapeOf", "[orchestrator][navkeys]")
{
    // Printable: "j" "G" "^" "0" "/";  Ctrl: "^d" "^u" "^c";  Sequence: "gg" "zz";  Special: "pgdn" "esc"
    // Invalid: "" " " "^h" "^i" "^j" "^m" "^D" "^1" "ggg" "PgDn" "\t" "\xc3\xa9"
}
TEST_CASE("navKeysOf: a configured list replaces the action's defaults", "[orchestrator][navkeys]")
{
    // configured[MoveDown] = {"J", "down"} → keysOf(MoveDown) == {"J","down"}; actionFor("j") empty.
    // configured[HalfPageDown] = {} → no half-page-down key, accepted.
    // VimKeys::Off with configured[MoveDown] = {"j"} → actionFor("j") == MoveDown.
    // configured[EnterField] = {"enter"} and configured[Close] = {"esc", "q"} → accepted (own defaults).
    // configured[LeaveField] = {"^c"} → actionFor("^c", Typing) == LeaveField.
    // configured[First] = {"0"} → accepted.
}
TEST_CASE("navKeysOf: what is refused, and the subject that names the other holder", "[orchestrator][navkeys]")
{
    // {MoveUp, {"^h"}}        → { MoveUp, 0, NotAKey }
    // {LeaveField, {"x"}}     → { LeaveField, 0, TypesText };  {LeaveField, {"gg"}} → TypesText
    // {Close, {}}             → { Close, nullopt, Unbound };   {LeaveField, {}} → Unbound
    // {MoveDown, {"e"}}       → Taken, "e, the dashboard's key to end the run"
    // {MoveDown, {"enter"}}   → Taken, "enter, the dashboard's key to start or open the selected row"
    // {LeaveField, {"^u"}}    → Taken, "^u, the dashboard's key to clear the text"
    // {HalfPageUp, {"^u"}}    → accepted (Navigating; ^u clears only while typing)
    // {Close, {"j"}}          → { Close, 0, Taken, "j, also keys.move_down" }
    // {Last, {"g"}}           → { Last, 0, Taken, "g, which starts keys.first's gg" }
    // {MoveDown, {"ee"}}      → Taken, "ee, which starts with the dashboard's key to end the run"
    // {MoveDown, {"J"}}, {Last, {"J"}} → reported at Last (later row): "J, also keys.move_down"
}
```

  `Config_test.cpp` (`[config]`), using its `Input { .find, .replace }` helper:
  - `keys` absent: `config->keys() == NavKeys::defaults(VimKeys::On)`.
  - `"keys": { "vim": false }`: `== NavKeys::defaults(VimKeys::Off)`.
  - `"keys": { "vim": false, "move_down": ["j"], "first": [0] }`: `keysOf(MoveDown) == {"j"}`,
    `keysOf(First) == {"0"}`.
  - `"keys": { "close": ["j"] }`: error key `keys.close[0]`, `KeyTaken`, and
    `message(...) == "keys.close[0]: is already bound: j, also keys.move_down"`.
  - `"keys": { "close": [] }`: `keys.close` with `Empty`.
  - `"keys": { "jump": ["x"] }`: `keys.jump` with `UnknownKey`.
  - `"keys": { "vim": "no" }`: `keys.vim` with `WrongType`; `"keys": { "move_up": "k" }`: `keys.move_up` with
    `WrongType`; `"keys": ["j"]`: `keys` with `WrongType`.
  - Review Focus 2: six tasks keyed `q`, `g`, `G`, `h`, `l` and `i` parse.
- [ ] **Step 2:** Build, then run `orchestrator_test.exe "[navkeys]"` and `"[config]"`. Expected: fail.
- [ ] **Step 3: Implement** `NavKeys.*` (a key listed twice in one action is kept once), `commandKeys()`, the JSON
  shape and field tables, the three `JsonType`s, the problems and their texts, and `Config::keys()`. Add the files
  to `CMakeLists.txt`.
- [ ] **Step 4:** Rebuild. Run `orchestrator_test.exe` (all), then
  `ctest --test-dir out/build/clangcl-release -R orchestrator`. Run the portability scripts and `clang-format -i`.
- [ ] **Step 5: Docs.**
  - README "Configuration" gains a `keys` subsection with this YAML. Give each action and its defaults in a table
    (from `navActionRows()`), then the clash rule and its example line, and say to quote a key YAML reads otherwise.

```yaml
keys:
    vim: false              # arrows, Home/End, Esc and Enter only
    move_down: [down, "^n"] # a list replaces the action's defaults
```

  - `## Task 2` in `extensions-text.md` adds a `keys` row after `quick_replies`.
- [ ] **Step 6: Commit:** message `feat(orchestrator): a keys map for the navigation keys`.

### Task 3: The `work` configuration — folded groups and empty groups — Haiku

**Files:**
- Create: `extensions/orchestrator/core/WorkGroups.hpp` (moved from `WorkItems.hpp`, plus the new types)
- Modify: `extensions/orchestrator/core/WorkItems.hpp` (include `WorkGroups.hpp`; the moved definitions leave)
- Modify: `extensions/orchestrator/core/ConfigJson.hpp` (`JsonWork`), `extensions/orchestrator/core/Config.hpp/.cpp`
  (`ObjectKind::Work`, `WorkFields`, `UnknownGroup`, `Listing::Groups`, `Config::work()`)
- Modify: `extensions/orchestrator/CMakeLists.txt`, `extensions/orchestrator/README.md` ("Configuration", the
  Work view's group table: "folded until you open it" becomes "folded by default (`work.folded`)")
- Append: `extensions-text.md` (`## Task 3`)
- Test: `extensions/orchestrator/core/Config_test.cpp`

**Interfaces:**
- Consumes: `JsonType::Bool` and `JsonType::Object` (Task 2), `ObjectKind`, `Field`, `matchesMirror`.
- Produces:

```cpp
// WorkGroups.hpp (namespace orchestrator): WorkGroup, FoldDefault and SortByUpdateKey unchanged, moved here.
struct WorkGroupRow { WorkGroup group; std::string_view foldKey; std::string_view name;
                      std::string_view configName; FoldDefault fold; };
// WorkGroupRows' configName: needs_you, ready_to_merge, in_review, no_mr_yet, everything_else.
/// Whether the Work view shows a group with no item.
enum class EmptyGroups : uint8_t { Shown = 0, Hidden };
/// The `work` settings.
struct WorkSettings
{
    std::vector<WorkGroup> folded = {};      ///< The groups folded until the user unfolds them, in row order.
    EmptyGroups empty = EmptyGroups::Hidden; ///< `fold_empty`.
    [[nodiscard]] bool operator==(WorkSettings const&) const = default;
};
/// @return The groups whose row is FoldDefault::Folded, and Hidden.
[[nodiscard]] WorkSettings defaultWorkSettings();

// Config.hpp
// ConfigProblem gains, last: UnknownGroup ///< A `work.folded` entry no WorkGroupRows row spells.
// Config gains: [[nodiscard]] WorkSettings const& work() const noexcept;  (Values::work = defaultWorkSettings())
```

  `JsonWork { std::optional<std::vector<std::string>> folded; std::optional<bool> foldEmpty; }`. The keys are
  `folded` and `fold_empty`, and `WorkFields` mirrors them (Strings, Bool). `work` is a `JsonType::Object` field of
  `ConfigFields`, after `keys`. The `UnknownGroup` text is "must be one of", listing the five `configName`s. A
  group listed twice is kept once, and `folded` comes out in row order.

- [ ] **Step 1: Write the failing tests** (`Config_test.cpp`, `[config]`):
  - `work` absent: `config->work() == WorkSettings { .folded = { WorkGroup::EverythingElse }, .empty =
    EmptyGroups::Hidden }`.
  - `"work": { "folded": ["everything_else", "needs_you"], "fold_empty": false }`: `folded == { NeedsYou,
    EverythingElse }` and `empty == Shown`.
  - `"work": { "folded": [] }`: `folded` empty.
  - `"work": { "folded": ["later"] }`: `work.folded[0]` with `UnknownGroup`. The message ends with
    `must be one of: needs_you, ready_to_merge, in_review, no_mr_yet, everything_else`.
  - `"work": { "fold_empty": "yes" }`: `work.fold_empty` with `WrongType`.
  - `"work": { "sort": "x" }`: `work.sort` with `UnknownKey`.
- [ ] **Step 2:** Build, then run `"[config]"`. Expected: the new cases fail.
- [ ] **Step 3: Implement.** Move the definitions without changing them, apart from the new `configName` column.
- [ ] **Step 4:** Rebuild and run `orchestrator_test.exe` (all), which must pass with the move alone. Then run
  `ctest … -R orchestrator`, the portability scripts and `clang-format -i`.
- [ ] **Step 5: Docs.**
  - README "Configuration" gains a `work` subsection: `folded` (default `[everything_else]`) and `fold_empty`
    (default `true`, which hides a group with nothing in it). A fold you make with `z`, `⏎` or a click wins and is
    kept across restarts (Task 4 makes that true).
  - `## Task 3` in `extensions-text.md` adds a `work` row after `keys`.
- [ ] **Step 6: Commit:** message `feat(orchestrator): work.folded and work.fold_empty`.

### Task 4: Folds as the user's choice; configured defaults; empty groups — Opus

**Files:**
- Modify: `extensions/orchestrator/core/Engine.hpp` (`FoldChoice`, `SetFold`, the `Input` variant,
  `foldChoiceOf`, `setFold`), `extensions/orchestrator/core/Engine.cpp` (the visitor row),
  `extensions/orchestrator/core/EngineFolds.cpp`
- Modify: `extensions/orchestrator/view/Keys.hpp` (`SetFoldAction`, `UiAction`),
  `extensions/orchestrator/view/WorkBoard.hpp/.cpp` (`isGroupFolded`, `foldWorkGroup`, `workLines`),
  `extensions/orchestrator/view/ViewFixtures.cpp`, `extensions/orchestrator/guest/Driver.cpp` (`inputOf`)
- Modify: `extensions/orchestrator/README.md` (the Work view's paragraph on folds)
- Create: `extensions/orchestrator/view/testdata/work-empty-groups-80x24.screen`
- Test: `extensions/orchestrator/core/Engine_test.cpp`, `extensions/orchestrator/view/WorkBoard_test.cpp`,
  `extensions/orchestrator/view/Dashboard_test.cpp` (scene), `extensions/orchestrator/guest/Driver_test.cpp`

**Interfaces:**
- Consumes: `WorkGroupRows` with `configName`, `WorkSettings`, `EmptyGroups`, `Config::work()` (Task 3).
- Produces:

```cpp
// Engine.hpp
/// What the user chose for a fold.
enum class FoldChoice : uint8_t { Open = 0, Folded };
/// The user folded or unfolded a group, whatever it was before: kept as `<group>#open` or `<group>#folded`.
struct SetFold { std::string group; FoldChoice choice = FoldChoice::Open; uint64_t monoMs = 0;
                 [[nodiscard]] bool operator==(SetFold const&) const = default; };
// Engine:
/// @return The choice SetFold stored for @p group; std::nullopt: none.
[[nodiscard]] std::optional<FoldChoice> foldChoiceOf(std::string_view group) const noexcept;
// private: void setFold(std::string_view group, FoldChoice choice, std::vector<Effect>& out);
//   removes `group`, `group#open`, `group#folded`, then appends the chosen key; refused (nothing changes) for a
//   group that is not a fold key, or whose `#folded` key exceeds MaxFoldKeyBytes; one StoreSet on FoldsKey.

// Keys.hpp
/// Fold or unfold a Work-view group as the user chose (Decision 12).
struct SetFoldAction { std::string group = {}; FoldChoice choice = FoldChoice::Open;
                       [[nodiscard]] bool operator==(SetFoldAction const&) const = default; };
// UiAction gains SetFoldAction, last.

// WorkBoard.hpp
/// @return Whether @p group is folded: its SetFold choice, else a toggle stored before choices (against the
///         row's built-in FoldDefault), else Config::work().folded (the built-in default without a configuration).
[[nodiscard]] bool isGroupFolded(Engine const& engine, WorkGroup group) noexcept;
/// @return SetFoldAction of the group's fold key, choosing the opposite of what isGroupFolded() says now.
[[nodiscard]] UiAction foldWorkGroup(WorkState& state, Engine const& engine, WorkGroup group);
// workLines(): with Config::work().empty == Shown and the Importance sort, every group's heading, even with no
// item shown; Hidden keeps today's behaviour.
```

- [ ] **Step 1: Write the failing tests.**
  - `Engine_test.cpp`, new case `"Engine: a fold the user sets replaces the group's toggle"`:
    - after `ToggleFold { "work/else" }`, then `SetFold { "work/else", Open }`: `isFolded("work/else")` is false,
      `foldChoiceOf("work/else") == Open`, and the last `StoreSet` on `FoldsKey` has the value `["work/else#open"]`;
    - then `SetFold { …, Folded }` stores `["work/else#folded"]`;
    - a fresh engine that loads that value has `foldChoiceOf == Folded`;
    - a `SetFold` whose group is 199 bytes changes nothing.
  - `WorkBoard_test.cpp`, `"Work view: the configured folds, the user's choice, a fold stored before"` (Review
    Focus 5):
    - With the default configuration, `isGroupFolded(EverythingElse)` is true and `isGroupFolded(NeedsYou)` is
      false.
    - With `work: { folded: [in_review] }`, `InReview` is folded and `EverythingElse` is open.
    - With that configuration and the store `["work/else"]` (a toggle from before), `EverythingElse` is open, as
      the user left it.
    - With the store `["work/in-review#open"]` and `folded: [in_review]`, `InReview` is open. The same store with
      `folded: []` also leaves it open: the user's choice survives the default changing.
    - `foldWorkGroup(state, engine, InReview)` while it is folded returns
      `SetFoldAction { "work/in-review", FoldChoice::Open }`.
  - `"Work view: fold_empty false shows every group's heading"`: on a world whose items are all Needs you,
    `workLines` has five headings with `fold_empty: false` and one with the default.
  - `Driver_test.cpp`: `SetFoldAction` reaches the engine as `SetFold`, as the existing `ToggleFoldAction` case
    does.
- [ ] **Step 2:** Build, then run `"[engine]"`, `"[workboard]"` and `"[driver]"` (the tags those files use).
  Expected: fail.
- [ ] **Step 3: Implement.** Every caller of `foldWorkGroup` (`Fold`, `⏎` on a heading, the row menu's *Fold
  group*, a click on a heading, `foldOtherGroups`) now goes through the new signature. `foldOtherGroups` emits
  `SetFoldAction { …, Folded }`. The sort chip keeps `ToggleFoldAction { SortByUpdateKey }`.
- [ ] **Step 4: Scene.** Add `Scene { .name = "work-empty-groups", .world = <workWorld with work: { fold_empty:
  false }>, .keys = "w", .sizes = { Medium } }`, giving the view fixtures' world a configuration override if it
  has none. Regenerate the goldens. Only the new screen may appear; every existing screen must stay unchanged.
- [ ] **Step 5:** Rebuild and run `orchestrator_test.exe` (all), `ctest … -R orchestrator`, the portability
  scripts and `clang-format -i`.
- [ ] **Step 6: Docs.** In README "The Work view", the sentence on folds says that a fold you make wins over
  `work.folded` and is kept across restarts. With `fold_empty: false` every group shows its heading.
- [ ] **Step 7: Commit:** message `feat(orchestrator): Work folds are the user's choice over work.folded`.

### Task 5: The key resolver; the Fleet, the Focus and the Work list on it — Opus

**Files:**
- Create: `extensions/orchestrator/view/NavInput.hpp`, `extensions/orchestrator/view/NavInput.cpp`,
  `extensions/orchestrator/view/NavInput_test.cpp`
- Modify: `extensions/orchestrator/view/Keys.hpp/.cpp`:
  - the `KeyTable`/`WorkKeyTable` rows `up k down j home end left right` go;
  - `CtrlLetters` becomes every letter but `h i j m`;
  - `KeyCommand` gains `HalfPageUp` and `HalfPageDown` before `Reply1`;
  - `Interaction::pendingKey`, `KeyContext::fleetRows`;
  - `applyKey` and `keyNameOf`/`workKeyNameOf` get new signatures.
- Modify: `extensions/orchestrator/view/WorkBoard.hpp/.cpp` (`applyWorkKey`'s signature, half-page handlers,
  `RepoPrevious`/`RepoNext` on the sort chip), `extensions/orchestrator/view/Dashboard.cpp` (routing, `fleetRows`,
  dropping `pendingKey` on a mouse event), `extensions/orchestrator/view/Actions.cpp` and `view/Palette.cpp`
  (`keyNameOf` callers)
- Modify: `extensions/orchestrator/CMakeLists.txt`, `extensions/orchestrator/README.md` (the Fleet's and the Work
  view's key tables)
- Test: `extensions/orchestrator/view/Keys_test.cpp`, `extensions/orchestrator/view/WorkBoard_test.cpp`,
  `extensions/orchestrator/view/Dashboard_test.cpp`

**Interfaces:**
- Consumes: `NavAction`, `NavContext`, `NavKeys`, `NavKeys::defaults`, `commandKeys()`, `Config::keys()` (Task 2).
- Produces (namespace `orchestrator::view`):

```cpp
// NavInput.hpp
/// Whether a box's text field has the keys.
enum class FieldMode : uint8_t { Navigating = 0, Typing };
/// Why a key resolved to no action.
enum class NavMiss : uint8_t { NotBound = 0, Pending };
/// @return The name a binding gives @p key: a printable character, `^<letter>`, or a SpecialKeyNames name;
///         empty for a key with Alt, or one no name is given.
[[nodiscard]] std::string keyNameFor(core::tui::KeyEvent const& key);
/// @return The keys in force: the configuration's, else NavKeys::defaults(VimKeys::On) (one static instance).
[[nodiscard]] NavKeys const& keysInForce(Engine const& engine) noexcept;
/// Resolves a key for the surface that has the keys (Decision 7). A pending first key and @p key complete a
/// sequence, or the pending key is dropped and @p key is resolved alone; a key that starts a sequence becomes
/// the pending one (NavMiss::Pending: the key is taken, and nothing acts).
/// @param pending Interaction::pendingKey; set or cleared.
[[nodiscard]] std::expected<NavAction, NavMiss> navActionOf(core::tui::KeyEvent const& key, NavContext context,
                                                            NavKeys const& keys, std::string& pending);
/// @return The context of the surface that has the keys: Typing while the start dialog's or the `n` box's or the
///         palette is open (always typing), a filter is typed, or the message box has the keys; else Navigating.
[[nodiscard]] NavContext navContextOf(Interaction const& interaction, KeyContext const& context) noexcept;
/// @return The command @p action is in @p view's list (Decision 9's first and third columns); std::nullopt for
///         LeaveField, EnterField and Close, which no list command stands for.
[[nodiscard]] std::optional<KeyCommand> listCommandOf(NavAction action, View view) noexcept;
/// @return How a hint names @p action's first key (`↑`, `j`, `^d`); EnterField's first key other than `enter`;
///         empty: no such key.
[[nodiscard]] std::string navKeyLabel(NavKeys const& keys, NavAction action);

// Keys.hpp
// Interaction gains: std::string pendingKey = {}; ///< The first key of a sequence, until the next key.
// KeyContext gains:  std::size_t fleetRows = 0;   ///< How many Fleet rows were shown, as last drawn.
[[nodiscard]] std::optional<UiAction> applyKey(core::tui::KeyEvent const& key, std::optional<NavAction> nav,
                                               Interaction& interaction, KeyContext const& context);
[[nodiscard]] std::string keyNameOf(KeyCommand command, NavKeys const& keys);     // nav-mapped: navKeyLabel()
[[nodiscard]] std::string workKeyNameOf(KeyCommand command, NavKeys const& keys); // likewise
// WorkBoard.hpp
[[nodiscard]] std::vector<UiAction> applyWorkKey(core::tui::KeyEvent const& key, std::optional<NavAction> nav,
                                                 Interaction&, std::span<WorkLine const>, std::span<WorkItem const>,
                                                 Engine const&, std::size_t pageRows);
```

  `keyNameOf` and `workKeyNameOf` return `std::string` now, because a nav key's label is built from the
  configuration. `applyKey` and `applyWorkKey` act on `nav` through `listCommandOf` when it holds a movement action.
  Otherwise they use the command table as today. While the timeline is shown, `nav == Close` closes it.
  `Dashboard::State::applyKeys` resolves once per key for a list surface (no dialog, box, palette or menu open).
  On `Pending` it continues to the next key. Tasks 6 and 7 extend the same routing to the boxes.

- [ ] **Step 1: Write the failing tests.**
  - `NavInput_test.cpp` (`[orchestrator][view][navinput]`):
    - `keyNameFor`: `j` gives `"j"`, Shift+`g` (codepoint `G`) gives `"G"`, Ctrl+D gives `"^d"`, ↓ gives `"down"`,
      Alt+`j` gives `""`, and Ctrl+H gives `""`.
    - `navActionOf` with the defaults, in Navigating:
      - `j` gives MoveDown;
      - `g` gives `Pending` with `pending == "g"`; then `g` gives First with `pending` empty;
      - Review Focus 4: `g` then `j` gives MoveDown with `pending` empty;
      - `x` gives NotBound.
    - In Typing, `j` gives NotBound (typed by the surface) and `esc` gives LeaveField.
    - `listCommandOf(NextValue, View::Work) == RepoNext`, `listCommandOf(NextValue, View::Agents) == NextPage`,
      `listCommandOf(Close, View::Work)` is empty.
    - `navKeyLabel(defaults(On), MoveDown) == "↓"`, `navKeyLabel(defaults(On), EnterField) == "i"`,
      `navKeyLabel(defaults(Off), EnterField)` is empty.
    - Every key in `keyRows()` and `workKeyRows()` is a `commandKeys()` name in context Navigating. No row of
      either table holds a key any `navActionRows()` default holds in Navigating.
  - `Keys_test.cpp`:
    - In the Fleet, `G` selects the last row, then `gg` the first.
    - Ctrl+D with `fleetRows = 10` moves the selection by 5, and Ctrl+U moves it back.
    - In the Focus without a message box, `j` scrolls a line and Ctrl+D scrolls `rows / 2`.
    - `h`/`l` turn the transcript's pages, as `←`/`→` do.
    - `q` closes the timeline; `q` in the Fleet with nothing open does nothing.
    - With `keys: { vim: false }`, `j` does nothing in the Fleet.
  - `WorkBoard_test.cpp`:
    - `l`/`h` step the repository chip.
    - With the sort chip focused, `l` toggles the sort (one `ToggleFoldAction { SortByUpdateKey }`).
    - Ctrl+D moves the selection by `pageRows / 2`. `G` goes to the last line, then `gg` to the first.
    - The bar still names `→ repository` with the defaults.
- [ ] **Step 2:** Build, then run `"[navinput]"`, `"[keys]"` and `"[workboard]"`. Expected: fail.
- [ ] **Step 3: Implement.** Set `fleetRows` where the Dashboard records the Fleet's drawn height. Update every
  caller of the changed signatures, tests included. Tests that call `applyKey`/`applyWorkKey` resolve `nav` with a
  shared helper in `ViewFixtures` (`navFor(key, context)`, defaults).
- [ ] **Step 4:** Regenerate the goldens. Bars use the first key, plain first, so no screen should change. Read any
  screen that does, and explain or fix it. Rebuild and run `orchestrator_test.exe` (all), `ctest … -R orchestrator`,
  the portability scripts and `clang-format -i`.
- [ ] **Step 5: Docs.**
  - The README key tables gain `j` `k` `gg` `G` `Ctrl+D` `Ctrl+U` beside the arrows. The Work view's `←` `→` row
    becomes `←` `→` `h` `l`, stepping the chip with the keys. The timeline closes with `q` too.
  - Add one line: "Every navigation key is configurable (`keys`, see Configuration)."
- [ ] **Step 6: Commit:** message `feat(orchestrator): vim keys in the lists, from one key table`.

### Task 6: Typing and navigating in the start dialog and the `n` box — Opus

**Files:**
- Modify: `extensions/orchestrator/view/StartDialog.hpp/.cpp` (`StartDialogState::mode`, `applyStartDialogKey`,
  the hint line, a click on the note), `extensions/orchestrator/view/TaskBox.hpp/.cpp` (`TaskBoxState::mode`,
  `applyTaskBoxKey`, the hint line, clicks), `extensions/orchestrator/view/Dashboard.cpp` (routing to both)
- Modify: `extensions/orchestrator/README.md` ("Starting an agent": the dialog's and the `n` box's keys)
- Create: `view/testdata/start-dialog-keys-{80x24,120x40}.screen` and `view/testdata/task-box-settings-{50x20,80x24}.screen`
- Modify: `view/testdata/start-dialog-*.screen` and `view/testdata/task-box-*.screen` (the hint line)
- Test: `extensions/orchestrator/view/StartDialog_test.cpp`, `extensions/orchestrator/view/TaskBox_test.cpp`,
  `extensions/orchestrator/view/Dashboard_test.cpp` (scenes)

**Interfaces:**
- Consumes: `FieldMode`, `navActionOf`, `navContextOf`, `keysInForce`, `navKeyLabel` (Task 5); `NavAction` (Task 2).
- Produces:

```cpp
// StartDialog.hpp: StartDialogState gains  FieldMode mode = FieldMode::Navigating; ///< Typing: the note has the keys.
[[nodiscard]] std::optional<DispatchAction> applyStartDialogKey(core::tui::KeyEvent const& key,
    std::optional<NavAction> nav, std::optional<StartDialogState>& dialog, Engine const& engine,
    HooksWarning hooksAtInput, std::string_view nearTab);
// TaskBox.hpp: TaskBoxState gains  FieldMode mode = FieldMode::Typing; ///< Typing: the text has the keys.
[[nodiscard]] std::optional<DispatchAction> applyTaskBoxKey(core::tui::KeyEvent const& key,
    std::optional<NavAction> nav, std::optional<TaskBoxState>& box, Engine const& engine,
    HooksWarning hooksAtInput, std::string_view nearTab);
```

  The rules come from Decision 9's "Start dialog / `n` box" column, Decisions 6 and 11, and the following:
  - **Typing.**
    - Only the line-editing keys and printable keys edit the field.
    - `⏎` confirms, as today.
    - `Tab`/`Shift+Tab` move to the next or previous setting. Arriving on the text line by `Tab` or a click
      types; arriving by `j`/`k` only selects it.
    - `leave_field` turns the mode to Navigating. The dialog stays on Note, and the `n` box moves its focus to
      Repo.
  - **Navigating.** No printable key types. `Ctrl+P` toggles the dialog's preview in both modes.
  - **Hints.**
    - Dialog, navigating: `⏎ start · ↑↓ settings · ←→ change · i note · esc close`.
    - Dialog, typing: `⏎ start · esc settings`.
    - `n` box, navigating: `⏎ start · ↑↓ move · ←→ change · i text · esc close`.
    - `n` box, typing: `⏎ start · esc settings`.
    - Each key is `navKeyLabel()` of its action (`↑`+`↓`, `←`+`→`, EnterField, Close, LeaveField). A hint whose
      key is empty is left out. The `⏎` hint keeps today's `enterHint()`.

- [ ] **Step 1: Write the failing tests.**
  - `StartDialog_test.cpp`, `"start dialog: jk move between the settings, hl change one, letters type nothing"`:
    - The dialog opens on Task, Navigating.
    - `j` four times reaches Worker, Model, Perms and Note, and a fifth `j` stays on Note. `k` goes to Perms,
      `gg` to Task and `G` to Note.
    - On Task, `l` changes the task as `→` does and `h` changes it back.
    - `x` on Task leaves the note `""`, and `q` closes the dialog.
    - For a worker that takes no profile, `j` from Model goes to Note.
  - `"start dialog: i types into the note; esc leaves it, a second esc closes"` (Review Focus 3):
    - `i` goes to Note, Typing. Typing `jkhlqgGi` gives the note `"jkhlqgGi"`.
    - `esc` goes to Navigating with the dialog open; a second `esc` closes it.
    - Reopened: `G` then `⏎` goes to Typing on Note, and a second `⏎` returns the dispatch, as today's `⏎`.
    - Alt+`j` while typing goes to Navigating with the note unchanged and the dialog open.
    - `Tab` from Model reaches Note, Typing.
  - `TaskBox_test.cpp`, `"n box: opens typing; esc goes to the settings; a second esc closes"`:
    - The box opens on Text, Typing. Typing `fix jkhlqgGi` gives exactly that text.
    - `esc` goes to Repo, Navigating. `j` goes to Worker, and `l` steps the worker as `→` does.
    - `k` `k` reaches Text, Navigating, where `x` leaves the text unchanged.
    - `⏎` on Text goes to Typing, and `esc` goes back to Repo.
    - `i` goes to Text, Typing. `esc` then `q` closes the box.
    - Typing then `⏎` at once still dispatches (the one-step flow).
- [ ] **Step 2:** Build, then run `"[startdialog]"` and `"[taskbox]"`. Expected: fail.
- [ ] **Step 3: Implement**, with `Dashboard::State::applyKeys` resolving `nav` for both boxes from
  `navContextOf`.
- [ ] **Step 4: Scenes.**
  - `Scene { .name = "start-dialog-keys", .world = workWorld(), .keys = "w\rjjl", .sizes = { Medium, Wide } }`
    shows Model stepped, Navigating.
  - `Scene { .name = "task-box-settings", .world = World {}, .keys = "ninvestigate the flaky test\x1b", .sizes =
    { Narrow, Medium }, .then = "j" }` shows Worker selected, Navigating.

  If the scene harness does not resolve a trailing lone `Esc` before `.then` (`Dashboard`'s `EscapeTimeout`),
  advance its clock past the timeout there, as the Dashboard's own Esc tests do. Regenerate the goldens. The
  existing start-dialog and task-box screens may change only on their hint line.
- [ ] **Step 5:** Rebuild and run `orchestrator_test.exe` (all), `ctest … -R orchestrator`, the portability
  scripts and `clang-format -i`.
- [ ] **Step 6: Docs.**
  - README "Starting an agent": the dialog opens on its settings. `j`/`k` (or `↑`/`↓`, `Tab`) move, `h`/`l` (or
    `←`/`→`) change, `i` types the note, `Esc` leaves the note, and a second `Esc` or `q` closes. `⏎` on the Note
    line types, and elsewhere it starts.
  - The `n` box opens typing. `Esc` goes to its settings, where `hjkl` work, and `i` or `⏎` on the text types
    again. A second `Esc` or `q` closes. Typing then `⏎` still starts at once.
- [ ] **Step 7: Commit:** message `feat(orchestrator): vim keys in the start dialog and the n box`.

### Task 7: The palette, the row menu, the filters and the message box — Opus

**Files:**
- Modify: `extensions/orchestrator/view/Palette.hpp/.cpp` (`applyPaletteKey`: `Ctrl+J`/`Ctrl+K`),
  `extensions/orchestrator/view/RowMenu.hpp/.cpp` (`applyRowMenuKey`), `extensions/orchestrator/view/Keys.hpp/.cpp`
  (`typeIntoFilter`, the message box's leave key, Alt), `extensions/orchestrator/view/WorkBoard.cpp`
  (the filter), `extensions/orchestrator/view/Dashboard.cpp` (routing)
- Modify: `extensions/orchestrator/README.md` (the palette, the row menu sentence, the filters, the message box)
- Test: `extensions/orchestrator/view/Palette_test.cpp`, `extensions/orchestrator/view/RowMenu_test.cpp`,
  `extensions/orchestrator/view/Keys_test.cpp`, `extensions/orchestrator/view/WorkBoard_test.cpp`,
  `extensions/orchestrator/view/Dashboard_test.cpp`

**Interfaces:**
- Consumes: `FieldMode`, `navActionOf`, `navContextOf`, `navKeyLabel`, `Interaction::pendingKey` (Task 5).
- Produces:

```cpp
// applyPaletteKey keeps its signature: the palette consults no nav action (Decision 15).
[[nodiscard]] std::optional<RowMenuCommand> applyRowMenuKey(core::tui::KeyEvent const& key,
    std::optional<NavAction> nav, std::optional<RowMenuState>& menu, WorkItem const* item);
[[nodiscard]] FilterInput typeIntoFilter(core::tui::KeyEvent const& key, std::optional<NavAction> nav,
                                         Filtering& filtering, std::string& filter);
[[nodiscard]] FilterInput typeIntoFilter(core::tui::KeyEvent const& key, std::optional<NavAction> nav,
                                         Interaction& interaction);
```

  The rules come from Decisions 9, 10, 11 and 15:
  - **Palette** (Decision 15). Its hint stays today's: `⏎ run · ↑↓ choose · esc close`.
  - **Row menu.** It is always Navigating. Movement wraps nowhere: the highlight stops at the ends, as `↑`/`↓` do
    today. Its hint, if any, reads as today.
  - **Filter.** `leave_field` keeps the filter (Decision 10).
  - **Message box.** `leave_field`, not `KeyCode::Escape`, returns to the Fleet with the draft kept. Alt+Enter
    still adds a line, and any other key with Alt is `leave_field` (Decision 11).

- [ ] **Step 1: Write the failing tests.**
  - `Palette_test.cpp`:
    - `jkhlqgGi` types the query `"jkhlqgGi"` (Review Focus 3).
    - `j` typed into the query filters: on a query `re`, typing `j` leaves `paletteMatches` the entries matching
      `rej`, and the highlight is reset to 0.
    - With an empty query, `↓` highlights 1, Ctrl+J 2, Ctrl+K 1, `↑` 0, and `↑` again the last entry (wrap); the
      query stays `""` throughout.
    - Type `re`, then one `esc`: the palette is closed, and `paletteQuery == "re"`.
    - Ctrl+J does not run the highlighted command; `⏎` does.
  - `RowMenu_test.cpp`: `j` moves the highlight to 1, `G` to the last entry and `gg` to 0. `q` closes the menu.
    `↓` still moves the highlight.
  - `Keys_test.cpp`:
    - The Fleet's `/` then typing `jkq` gives the filter `"jkq"`. `esc` sets `Filtering::Off` with the filter
      `"jkq"` kept; a second `esc` clears it; `/` again types with the text kept.
    - The message box: typing `jkhlq` gives the draft `"jkhlq"`, and `esc` returns to the Fleet with the draft
      kept.
    - With `keys: { leave_field: ["^c"] }`, Ctrl+C leaves the message box and `esc` does not.
  - `WorkBoard_test.cpp`: the Work filter's `esc` keeps the filter, and the second `esc` clears it.
- [ ] **Step 2:** Build, then run `"[palette]"`, `"[rowmenu]"`, `"[keys]"` and `"[workboard]"`. Expected: fail.
- [ ] **Step 3: Implement**, with `Dashboard::State::applyKeys` resolving `nav` for the menu and a typed filter,
  not for the palette. The Work view's palette-`⏎` shortcut (`isWorkCommand`) keeps working.
- [ ] **Step 4: Goldens.** Regenerate. No palette screen and no `work-filter` screen may change.
- [ ] **Step 5:** Rebuild and run `orchestrator_test.exe` (all), `ctest … -R orchestrator`, the portability
  scripts and `clang-format -i`.
- [ ] **Step 6: Docs.**
  - README: in the palette every letter types into the query; `↑` `↓` or `Ctrl+J` `Ctrl+K` choose, `⏎` runs, and
    `Esc` closes it at once, keeping the query.
  - The row menu: `j`/`k`/`gg`/`G` choose and `q` closes.
  - The Work view's `/` row: `Esc` keeps the filter and a second `Esc` clears it (it cleared at once before).
  - The message box: `Esc` returns to the Fleet with the draft kept.
- [ ] **Step 7: Commit:** message `feat(orchestrator): vim keys in the row menu and the filters, Ctrl+J/K in the palette`.
