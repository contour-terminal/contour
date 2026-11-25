# Orchestrator: auto mode, vim keys, configurable defaults, and large repositories — design

Follows the Work board (docs/superpowers/specs/2026-11-25-orchestrator-work-board-design.md).

## Intent

After using the Work board the user asked for four things:

1. **Agents run in Claude Code's `auto` permission mode by default** — "this should be a default behaviour".
2. **Vim-like navigation** in every list, dialog and menu, "for instance when I create a new agent".
3. **Everything configurable**, with "nice customisations": the defaults above are defaults, not fixed.
4. **A finished agent on a large repository is no longer kept by mistake.** Every run on the user's large
   repository (≈21.5k tracked files) ends as *kept* with "the index check failed: its output was cut at the host's
   limit": `git ls-files -v --stage` prints ≈2.1 MB, and the host keeps 64 KiB of a command's output
   (`vtpty::MaxCapturedOutput`). The worktree was clean and pushed; the check, not the work, kept it.

The user also wrote "automatically try to folder"; read here as **groups that fold by themselves**, made
configurable (see 3c). If that reading is wrong, 3c is dropped.

## Goals

1. **`auto` mode.** `auto` becomes a permission mode the orchestrator knows (`PermissionModeNames` gains a row),
   and the built-in default profile runs agents in `auto`. `bypassPermissions` stays refused.
2. **Vim keys**, everywhere keys move or change something and nothing is being typed:
   - `j` `k` move (lists, dialog settings, menu and palette entries); `h` `l` change the selected setting's value
     (start dialog, `n` box settings) and step a chip (Work view's repository / sort).
   - `gg` / `G` first / last line; `Ctrl+D` / `Ctrl+U` half a page; `q` closes a dialog, menu or palette like `Esc`.
   - In a text field (the `n` box's text, the dialog's note, the filter, the message box) letters type. `Esc`
     leaves the field — the keys go to the settings, where `hjkl` work — and `i` or `Enter` enters it again. A
     second `Esc` closes the box. The `n` box still opens with the cursor in its text, so type + `⏎` starts at once.
   - Arrow keys, `Tab`, `Home`/`End` and the mouse keep working unchanged.
3. **Configuration** (all optional; omitted = the defaults above):
   a. **Permissions.** The built-in profiles gain `auto` (mode `auto`) and it is the default profile; `standard`
      and the others stay selectable in the start dialog. `permission_profiles` may redefine any of them; the
      default profile is a setting (`default_permissions: <profile>`), per task overridable as today.
   b. **Keys.** A `keys` map rebinds the navigation actions (move up/down, first/last, half page up/down, change
      value previous/next, leave field, enter field, close). Each action takes a list of keys; `vim: false`
      removes the vim defaults and leaves arrows only. Bindings that clash with a dashboard key or with each
      other are refused with a problem line naming both, as task keys are today.
   c. **Folding.** `work.folded: [<group>, …]` names the Work view's groups folded until the user unfolds them
      (default `[everything_else]`), and `work.fold_empty: true` (default) hides a group with no items. A fold the
      user makes still wins and is kept across restarts.
4. **Large repositories.** The removal checks give a correct answer for a repository of any size, within the
   host's limits, without weakening what they protect: a worktree with assume-unchanged or skip-worktree
   entries, or a submodule, is still kept.

## Non-goals

- New permission semantics: the orchestrator passes the mode; Claude Code decides what `auto` allows.
- A full vim (counts, operators, marks, registers, visual mode).
- Reworking other checks than the index check, unless they share its cause.

## Anti-goals

- `bypassPermissions` becoming one line of configuration away (it stays refused).
- A vim key that types into a text field or triggers an action while the user is typing.
- A kept worktree that held work being removed because the check gave up: when the check cannot decide, the
  worktree is kept, and the reason says what to do.

## Approaches

**Index check (the only open "how"):**
- **A — a check git can answer briefly (recommended, builder to confirm).** Ask git only for the entries that
  matter instead of every entry. git has no "flagged entries only" switch and commands run without a shell, so
  the builder evaluates candidates such as `git submodule status`, `git config --get core.sparseCheckout`,
  `git sparse-checkout list`, `git ls-files --format=...` and pathspec magic, measures each on a ≥20k-file
  repository, and picks the smallest set that still detects assume-unchanged, skip-worktree and gitlinks.
  Host unchanged.
- **B — the host keeps more for a declared command.** A command line may declare a larger output class
  (e.g. `long` → 8 MiB) and the guest parses the listing. Simple, but every guest call carries megabytes and the
  host's per-call budget is 64 KiB today.
- **C — the host filters.** A declared command names a line filter the host applies before keeping output. Most
  general, most host change.

Recommended: **A**; fall back to **B** only if no git query detects all three conditions compactly. The builder
records the measurement either way.

**Vim keys:** a data-driven key table (action → default keys) consulted by every view's key handler, replacing
per-view arrow-key code; text fields switch between typing and navigating as in Goal 2. Chosen over per-view
additions because the user wants it configurable everywhere.

## Constraints

- Every configuration valid today stays valid; new keys are optional.
- Keyboard parity with the mouse stays; the `n` box's "type, ⏎" flow stays one step.
- The orchestrator stays free of Contour's `src/` unless approach B or C is chosen for the index check.
- Screen tests for: the dialog navigated with `jk`/`hl`; the `n` box after `Esc`; the README and
  `docs/extensions.md` (via the WIP-preserving procedure) document every new setting.

## Left to the builder

- The exact git query (or host option) for the index check, with the measurement.
- Key-table naming and file layout; how a clash is reported.
- The user's own `contour.yml` is updated to name `auto` explicitly once it ships (so the choice is visible).
