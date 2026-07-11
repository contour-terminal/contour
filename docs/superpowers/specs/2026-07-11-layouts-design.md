# Layouts: configurable startup tab/pane sets

**Status:** Design approved — ready for implementation planning
**Date:** 2026-07-11

## Summary

Add a new top-level `layouts:` configuration section to Contour. A *layout* is a named
set of tabs; each tab defines a command, working directory, name, and color, and may
contain a tree of split panes. Layouts can open automatically at startup, be launched
on demand from the CLI or a keybinding, and be **saved back** from a live window into a
machine-managed layouts file.

The user's motivating example: define a tab that changes into a project directory,
runs `claude`, and shows a chosen name and color — reproducibly, every time.

### Scope

**In scope (this spec):**
- Define layouts in config (tabs, per-tab command/dir/name/color/profile, recursive splits).
- Launch a layout three ways: startup default, CLI flag, keybinding action.
- Save the current window's live layout back into a separate `layouts.yml`.

**Out of scope (captured for a later, separate spec):**
- An **embedded GUI config editor** with a live-updating preview pane. The user wants
  this, but it is a distinct subsystem (an in-app text editor widget) and will be
  designed on its own. Note that Contour already has `--live-config` (file-watch +
  hot reload) and a `ReloadConfig` action, which that future feature will build on.

## Key decisions (from brainstorming)

| Decision | Choice |
| --- | --- |
| Launch model | Named sets **and** a startup default (both). |
| Command semantics | **Replace the shell** — a tab/pane's `command` becomes the child process's `program` (matches the existing `shell` field). |
| Profile per tab | Optional per-tab (and per-pane) `profile:` override; defaults to the window's profile. |
| Profile override scope | **Terminal-level only** (shell, fonts, colors, scrollback). Window-level settings (size, decorations) come from the window's launching profile. |
| Splits | Supported — recursive pane trees per tab. |
| Triggers | CLI flag **+** keybinding action **+** startup default (all three). |
| Keybinding target | **Append tabs to the current window.** |
| Naming | Top-level section is `layouts:`; startup default is `default_layout:`. |
| Save-to-config | Write to a **separate `layouts.yml`** sibling file; never rewrite the hand-authored `contour.yml`. |

## Architecture context (existing code)

Contour separates an authoritative, Qt-free model layer (`src/vtmux/`:
`SessionModel`, `Window`, `Tab`, `Pane`) from the Qt/QML view (`src/contour/`). PTY
spawning is isolated behind a `SessionFactory` interface. The relevant seams:

| Concern | Location |
| --- | --- |
| New-tab trigger | `Actions.h` `CreateNewTab` → `TerminalSession.cpp` → `TerminalSessionManager::createNewTab` |
| Core tab creation | `TerminalSessionManager::createSessionInBackground` + `createBackingSession` (`TerminalSessionManager.cpp`) |
| Shell/cwd/env resolution | `SessionFactory.cpp` — reads the active profile's `shell` `ExecInfo`; only cwd/size overridden per-session today |
| Split creation | `SessionModel::splitActivePane(TabId, SplitState, ratio)`; wired via `TerminalSessionManager::splitActivePane` |
| Tab title (runtime) | `vtmux::Tab::_runtimeTitle`; `SessionModel::setTabTitle/resetTabTitle` |
| Tab color (runtime) | `vtmux::Tab::_colors[TabColorSource]`; `SessionModel::setTabColor(TabId, TabColorSource, RGBColor)`. `TabColorSource {Application, User}` — User overrides Application. |
| Config parsing (yaml-cpp) | `Config.cpp` `YAMLConfigReader`; profiles at `Config.h` `TerminalProfile`, container `profiles` map |
| ExecInfo shape | `vtpty/Process.h`: `{ program, arguments, workingDirectory, env }` |
| Config path | `config::configHome()` (e.g. `~/.config/contour/`) |

**Key existing gap:** tab name and tab color live only as runtime model state, set
interactively (GUI) or by the app (DECAC). There is no config field for them, and a new
tab always clones the active profile's `shell` with an inherited cwd. This feature adds
config-driven name/color/command/profile and threads them through session creation.

## Section 1 — Config schema

New top-level `layouts:` (a map of named layouts) and `default_layout:` (a scalar),
siblings of `profiles:`.

### YAML shape

```yaml
layouts:
  work:
    tabs:
      - title: "editor"            # optional; seeds the tab name
        color: "#d75f00"           # optional; hex or palette name, sets User tab color
        directory: "~/proj/foo"    # optional cwd; defaults to profile/inherited
        command: "nvim"            # optional; replaces the shell (program). omit -> profile shell
        arguments: ["."]           # optional args for command
        profile: "dark-big"        # optional per-tab profile override (terminal-level only)

      - title: "claude"
        color: "green"
        directory: "~/proj/foo"
        command: "claude"

      - title: "servers"           # a SPLIT tab: use `split:` instead of leaf fields
        split:
          orientation: vertical    # vertical = side-by-side, horizontal = stacked
          panes:
            - { command: "npm run dev",  directory: "~/proj/foo", ratio: 0.6 }
            - split:                            # panes nest recursively
                orientation: horizontal
                panes:
                  - { command: "htop" }
                  - { command: "journalctl -f" }

default_layout: work               # optional; opens automatically at startup
```

Conventions: `orientation: vertical` = panes **side by side**; `horizontal` = **stacked**.
`command`/`arguments` mirror the profile `shell` field's naming.

### Data structures (`Config.h`)

```cpp
// A node in a tab's pane tree: EITHER a leaf (command/dir/profile) OR a split
// (orientation + children). `children` empty => leaf.
struct LayoutPane {
    // leaf fields:
    std::optional<std::string>              command;      // program; replaces shell
    std::vector<std::string>                arguments;
    std::optional<std::filesystem::path>    directory;    // cwd
    std::optional<std::string>              profile;      // per-pane profile override
    double                                  ratio = 0.5;  // size weight within parent split
    // split fields:
    vtmux::SplitState                       orientation = vtmux::SplitState::Vertical;
    std::vector<LayoutPane>                 children;
};

struct LayoutTab {
    std::optional<std::string>            title;
    std::optional<vtbackend::RGBColor>    color;
    std::optional<std::string>            profile;   // tab-level default profile
    LayoutPane                            root;      // leaf for single-pane, split node otherwise
};

struct Layout { std::vector<LayoutTab> tabs; };

// on Config:
ConfigEntry<std::unordered_map<std::string, Layout>, documentation::Layouts> layouts {};
ConfigEntry<std::string, documentation::DefaultLayout> defaultLayout {};   // "" = none
```

Parsing follows the existing `YAMLConfigReader::loadFromEntry` pattern; a leaf's
`command`+`arguments`+`directory` reuse the same reader logic as the profile `shell`
`ExecInfo`. Color reuses the config's existing color parsing. Add `documentation::Layouts`
and `documentation::DefaultLayout` entries to `ConfigDocumentation.h`.

## Section 2 — Launch flow & core plumbing

### Core plumbing change (the one structural change)

Today `SessionFactory::createPty` always reads the active profile's `shell` and only
lets the caller override cwd. Widen the seam so a session can be created with a specific
command and profile:

- Add an optional session spec to `createBackingSession` / the factory:
  `{ std::optional<ExecInfo> commandOverride; std::optional<std::string> profileName; std::optional<PageSize> }`.
  - When `commandOverride` is set, the factory uses that `program`/`arguments`,
    falling back to the resolved profile's `shell` for any unset field and for `env`.
  - When `profileName` is set, resolve **that** profile instead of `_app.profileName()`.
  - Existing callers pass `nullopt` → behaviour is byte-for-byte unchanged (no regression).

### Realizing a layout in a window

`TerminalSessionManager::applyLayout(WindowId, Layout const&)`:

1. For each `LayoutTab`, resolve the effective profile (tab `profile:` → else window profile).
2. Create the tab's **root pane** session via the widened factory using the root leaf's
   command/dir/profile; `_model->createTab(window)`.
3. Apply `setTabTitle` and, if `color` set, `setTabColor(User, …)` to the new tab.
4. If the root is a **split node**, realize the tree by DFS: `splitActivePane(tabId,
   orientation, ratio)` creates a sibling from the *active* leaf, so activate the correct
   leaf before each split and seed each new leaf's session from its `LayoutPane`.
   (The recursive split algorithm — leaf activation order, `ratio` mapping — is the
   fiddliest part; the plan will spell it out. Runtime primitives all exist:
   `splitActivePane(tabId, direction, ratio)`, per-pane session creation via the widened
   factory, and a model call to set the active pane.)
5. Activate the first tab.

### Three triggers → one `applyLayout`

- **Startup default:** after config load, if `default_layout` names a valid layout,
  populate the initial window from it **instead of** creating the lone default tab
  (so there is no stray extra tab). Unset/invalid → today's single-default-tab behavior.
- **CLI:** `contour --layout NAME` (a flag on the terminal subcommand, parsed alongside
  `--live-config`/`config`). Opens NAME in the launched window. Unknown name → stderr
  error + non-zero exit.
- **Keybinding:** a new parameterized action `LaunchLayout{ std::string name }` (like
  `SwitchToTab{int}`) added to `Actions.h` + dispatch in `TerminalSession`. **Appends the
  layout's tabs to the current window.** Unknown name → logged warning + no-op.

## Section 3 — Save current layout → config

New action `SaveLayout{ std::string name }` (plus a matching entry point) that serializes
the active window's live tab/pane tree into `layouts.<name>` and writes `layouts.yml`.

### What is captured per tab/pane

- **title** ← the tab's runtime title, only if explicitly set (else omit → stays dynamic).
- **color** ← the tab's `User` color, only if set.
- **directory** ← each pane's live `session->workingDirectory()` (current cwd — the
  useful thing to restore).
- **command / arguments** ← the pane's *originally spawned* `ExecInfo` program/args. We
  record what the pane was launched with (from the layout or the profile shell). We do
  **not** snapshot the current foreground process (unknowable/fragile). A saved layout
  restores the same starting command in the same directory.
- **split tree** ← walked from the `vtmux::Pane` layout (orientation + ratios).
- **profile** ← the pane's profile if it differs from the window's.

### Separate-file strategy

Contour currently loads a single config file (no include directive). Introduce a sibling
**`layouts.yml`** in `configHome()`, auto-loaded and merged with any `layouts:` defined
inline in `contour.yml`. `SaveLayout` writes **only** `layouts.yml`, leaving the
hand-authored `contour.yml` untouched. Write via temp-file + atomic rename.

## Section 4 — Error handling & edge cases

- **Two layout sources merge:** inline `contour.yml` `layouts:` + auto-loaded
  `layouts.yml`. On name collision, `layouts.yml` wins (freshest `SaveLayout` output);
  log an info line. Both feed the same `config.layouts` map.
- **Unknown layout name:** CLI → stderr error + non-zero exit. Keybinding → warning + no-op
  (never crash). `default_layout` missing → warn, fall back to single default tab.
- **Unknown/invalid `profile:`:** warn, fall back to the window's profile (reuse the
  existing fallible `profile(name)` lookup returning nullptr on miss).
- **Bad `color:`:** warn, skip color (tab keeps default coloring).
- **Empty layout / empty `tabs:`:** treat as "no layout" → single default tab, warn.
- **`command` fails to exec:** same as a bad `shell:` today — PTY child exits, pane shows
  exit state. No special handling.
- **Invalid split spec** (single child under `panes:`, `ratio` ≤ 0): warn, degrade
  gracefully (single child → leaf; bad ratio → 0.5).
- **`live-config` reload:** re-reads the `layouts` map so subsequently launched layouts
  use fresh definitions; already-open tabs are **not** retroactively rearranged
  (consistent with profile reloads).
- **`SaveLayout` overwriting an existing name:** overwrites that entry, logs info.
- **`layouts.yml` unwritable:** error notification, no partial write (temp + atomic rename).

## Section 5 — Testing

Following existing Catch2 tests and the Qt-free model layer; keep as much as possible
Qt-free.

1. **Config parsing (Qt-free):** YAML strings → assert `config.layouts` — leaf tabs,
   split tabs, deeply nested splits, per-tab/per-pane `profile`, colors, omitted-field
   defaults, and the `contour.yml` + `layouts.yml` merge with collision precedence.
2. **Layout realization algorithm (Qt-free):** factor tree-building into a pure helper
   that turns a `Layout` into an ordered op sequence (`createTab`,
   `splitActivePane(dir, ratio)`, `seedPane(execInfo)`, `setTitle`, `setColor`,
   `activate`) driven against `SessionModel` with a **fake `SessionFactory`**. Assert the
   resulting pane tree, ratios, titles, colors — single-pane, side-by-side, stacked,
   recursive.
3. **Save/serialize (Qt-free):** constructed model window + known per-pane `ExecInfo`s →
   assert the produced `Layout` struct and emitted YAML (titles only when set, `User`
   color only when set, current cwd captured, split tree + ratios preserved).
4. **Round-trip property test (strongest guard):** `Layout → realize into model →
   serialize back → assert structural equality`. Catches drift between read and write
   paths — the main risk of a two-direction schema.
5. **Error-path tests:** unknown layout name, unknown profile ref, bad color, empty
   `tabs:`, degenerate split — assert graceful fallback + no crash.

Build **test-first (TDD)**: parsing tests before the parser, realization tests before
`applyLayout`, serialize tests before the save path.

## Documentation

- Add a `docs/configuration/` page for layouts (schema, examples, the three launch modes,
  save-to-config).
- Add `documentation::Layouts` / `documentation::DefaultLayout` doc strings in
  `ConfigDocumentation.h` (used for both default-config emission and the web docs).
- Add a changelog entry.

## Follow-up (separate specs)

1. **Embedded GUI config editor** with a live-updating preview pane (the user's desired
   form). Built on `--live-config` / `ReloadConfig`. Distinct subsystem — own spec.
