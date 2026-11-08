# Platform chord parity — design

Date: 2026-11-08
Status: draft, awaiting approval

## Intent

A user moving between Linux/Windows and macOS should find every Contour command under the same key,
translated the way each platform expects: `Ctrl+Shift+X` on the desktop, `⌘X` on a Mac. A user new to
Contour should find the chords they already know from other applications (browsers, VS Code, iTerm2,
Ghostty, kitty, GNOME Terminal) without opening the configuration.

The trigger: on macOS, `⌘T` opens a tab and `⌘1…9` select one, but the command palette is `⇧⌘P`, not
the `⌘P` the `Primary` rule predicts. An audit of `src/contour/config/DefaultBindings.cpp` found that
this is one of several divergences, on both sides.

## The rule

1. Every application command that has a default chord on one platform has one on the other.
2. The chord is written once with the platform-neutral `Primary` modifier (Command on macOS,
   Ctrl+Shift elsewhere), so it translates mechanically.
3. A Mac row departs from the `Primary` translation only where macOS or a strong Mac convention already
   owns that `⌘` key. Each such departure carries a one-line comment naming the owner.
4. A command left deliberately unbound on one platform is listed, with its reason, in the parity test's
   exemption table (below). There are no silent gaps.

## Goals

- Close every gap in the audit below, on both platforms.
- Make parity a tested property, so the tables cannot drift apart again.
- Document the defaults as a side-by-side table.

## Non-goals

- Re-thinking chords that already exist and work on both sides (e.g. `⌘1…9` vs `Alt+1…0`,
  `⇧⌘↩` vs `Ctrl+Shift+Z` for pane zoom: both are each platform's convention).
- A new user-facing modifier spelling in `contour.yml`. `Primary` stays the only platform-neutral one.
- Global (system-wide) hotkeys, the quick terminal, vi-mode keys.

## Anti-goals (a failure even if it technically works)

- A new default that takes a chord a full-screen application (vim, tmux, htop) relies on, on the
  alternate screen, where it did not before. Rows that only make sense at a shell prompt stay
  `PrimaryScreenOnly`.
- A new default that silently reaches an existing user who bound that chord to something else. Fallback
  rows already guarantee an explicit binding wins; this must stay true.
- The menu bar or palette showing the "wrong" chord, e.g. `⇧⌘P` next to *Command Palette…* once `⌘P`
  is the primary chord.

## Decisions taken with the user

- **Both platforms** are brought to the same completeness, not only macOS.
- **Every new row is `both`** (`RowReach::WrittenAndFallback`): a freshly generated `contour.yml` lists
  it, *and* configurations that predate it get it as a fallback. Not `native2026Only`: these are fixes,
  not a new look, so they apply to the Classic edition too.
- **Command palette on macOS is `⌘P`**, and is the chord the menu bar and the palette show. `⇧⌘P` stays
  bound as a second chord for VS Code / Zed / Ghostty muscle memory.

## The audit and the changes

"D" = Linux/Windows (`DesktopOnly`), "M" = macOS (`MacOnly`). Chords in **bold** are new.

### macOS gaps and needless departures

| Command | Desktop today | Mac today | Mac after | Note |
|---|---|---|---|---|
| OpenCommandPalette | Ctrl+Shift+P | ⇧⌘P | **⌘P** + ⇧⌘P | `⌘P` (Print) is unused by Contour. `⌘P` is the displayed chord. |
| ClosePane | Ctrl+Shift+X | — (⌘W is CloseTab) | **⌘W** = ClosePane | iTerm2 / Ghostty: `⌘W` closes the focused split, and the tab with its last pane (ClosePane already does that). |
| CloseTab | — | ⌘W | **⇧⌘W** | Closes the whole tab regardless of splits. Safari uses ⇧⌘W for close window; acceptable since Contour's window close is the traffic light / last tab. |
| OpenCommandOutput | Ctrl+Shift+G | — (⌘G is find-next) | **⇧⌘O** ("open output") | `FallbackOnly`, `PrimaryScreenOnly`, `DeclinedInput::Consume`, exactly like the desktop row: only fallback rows can consume a declined press (see "Declined presses"). iTerm2 users know ⇧⌘O as Open Quickly, which Contour does not have. |
| ToggleStatusLine | Ctrl+Alt+. | — | **⌃⌘.** | See "the Ctrl+Alt rows" below. |
| OpenFileManager | Ctrl+Alt+O | — | **⌃⌘O** | |
| ScreenshotVT | Ctrl+Alt+S | — | **⌃⌘S** | |
| ToggleInputMethodHandling | Ctrl+Alt+, | — | **⌃⌘,** | |
| PasteClipboard(strip) | Ctrl+Alt+V | — | **⌃⌘V** | The ⌃⌘ tier. Not ⌥⇧⌘V: Option composes, and `DefaultBindings_test.cpp` ("no macOS default relies on Alt for a character key") forbids it. |

**The Ctrl+Alt rows.** On the desktop, the second tier of application chords is `Ctrl+Alt+letter`.
On a Mac, Option composes characters, so an Option+letter character row cannot fire (which is why these
rows are `DesktopOnly` today). The Mac's own second tier is `⌃⌘` (`⌃⌘F` full screen, `⌃⌘Space`
character viewer). So a Ctrl+Alt desktop row translates to a `⌃⌘` Mac row.
*Builder's call:* whether that translation is a second internal `ChordSpec` kind (beside `Primary`,
resolved per platform like `resolveChord`, and so one row instead of a Desktop/Mac pair) or explicit
`MacOnly` rows. The internal kind is preferred (data-driven: one row per command), but it must **not**
become a `contour.yml` spelling (see non-goals). The writer must still emit valid concrete modifiers
for it.

### Desktop gaps

| Command | Desktop today | Desktop after | Mac | Note |
|---|---|---|---|---|
| CopySelection + CancelSelection | Ctrl+C only while selecting | **Ctrl+Shift+C** (`Primary`, `both`) | ⌘C | Documented in `docs/configuration/key-mapping.md` but never bound. A written row cannot consume a declined press, but CopySelection with the default `CopyFallback::None` does not decline on an empty selection, so nothing leaks. |
| PasteClipboard | Ctrl+Shift+V | unchanged | ⌘V | Already a `Primary` row. Check the Mac fallback duplicate stays harmless. |
| ClosePane | Ctrl+Shift+X | **+ Ctrl+Shift+W** | ⌘W | GNOME Terminal / kitty / Windows Terminal close with Ctrl+Shift+W. Same pane-then-tab semantics as `⌘W`. Ctrl+Shift+X stays. |
| CloseTab | — | stays unbound | ⇧⌘W | On the desktop, `PrimaryShift` collapses to Ctrl+Shift, so it cannot be distinguished from ClosePane. Exempt, reachable via menu/palette/tab context menu. |
| IncreaseFontSize | Ctrl+wheel only | **Ctrl+Shift+=** and **Ctrl+Shift++** | ⌘+, ⌘= | kitty's chords. Shrinking is already Ctrl+Shift+- / Ctrl+Shift+_. |
| ResetFontSize | Ctrl+0 | unchanged | ⌘0 | (Ctrl+0 is not `Primary`; leave it, browsers use it.) |

### Mac-only commands brought to Linux/Windows

Decided with the user after the first review: converge with VS Code's integrated terminal, which binds
the command-navigation chords as `Ctrl+↑`/`Ctrl+Shift+↑` on Linux/Windows and `⌘↑`/`⇧⌘↑` on macOS,
exactly as Contour's Mac rows already do.

| Command | Mac today | Desktop after | Note |
|---|---|---|---|
| FocusPreviousCommand / FocusNextCommand | ⌘↑ / ⌘↓ | **Ctrl+↑ / Ctrl+↓** (`both`, `PrimaryScreenOnly`) | Free in the desktop table. On the primary screen only, so vim/tmux keep the chord. Risk: a user whose shell binds Ctrl+↑ loses it at the prompt unless they bind it in `contour.yml`. |
| SelectPreviousCommandOutput / SelectNextCommandOutput | ⇧⌘↑ / ⇧⌘↓ | **Ctrl+Shift+↑ / Ctrl+Shift+↓** (`both`, `PrimaryScreenOnly`) | **Replaces** ScrollMarkUp/Down on this chord on Linux/Windows (user's decision). ScrollMarkUp/Down stay on Ctrl+Alt+K / J there. |
| ClearToPrompt | ⌘K | **Ctrl+Shift+L** (`FallbackOnly`, `PrimaryScreenOnly`, `Consume`) | The shell's Ctrl+L plus Shift = the terminal-side clear that also drops scrollback. Ctrl+Shift+K (the literal translation) is NewWindowTo. Fallback-only like ⌘K, so a declined press does not type an L. |

**The ScrollMark row.** Today `fallback(All, CtrlShift, Key::UpArrow/DownArrow, PrimaryScreenOnly,
ScrollMarkUp/Down)` exists on every platform. It becomes `MacOnly` (Mac users keep ⌃⇧↑/↓ unchanged;
⌃+arrows are rarely used there because Mission Control owns ⌃↑), and the desktop gets the
SelectCommandOutput rows on that chord. Because the old row is fallback-only, the change reaches every
existing desktop user: it is a deliberate behaviour change and goes in the release notes. Row order must
put the new desktop rows where `BindingCollisions` stays green.

### Kept departures (documented, not changed)

Each gets a comment at its row if it does not have one already:

- `⇧⌘H` (NoSearchHighlight): `⌘H` hides the application.
- `⇧⌘K` (NewWindowTo) and `⌘K` (ClearToPrompt): `⌘K` is the Mac terminal clear.
- `⌘D` / `⇧⌘D` (splits): iTerm2's chords. Desktop uses Ctrl+Shift+E / O.
- `⌘G` / `⇧⌘G` / `⌘E`: the macOS find chords.
- `⌘Q`: owned by the menu bar (Quit); `Ctrl+Shift+Q` desktop-only.
- `⌘1…9` vs `Alt+1…0`: each platform's tab convention.

### Parity exemptions (the test's table)

Commands bound on one platform only, with the reason the test prints:

- Quit — macOS menu bar owns `⌘Q`.
- SpeakSelection — macOS has a system-wide Speak Selection (`⌥Esc`).
- CloseTab — desktop: indistinguishable from ClosePane's chord (see above).
- FocusToolbar — macOS-only concept (AppKit's ⌃F5).
- SearchSelection (⌘E) — a macOS find-pasteboard convention with no Linux/Windows equivalent; its literal
  translation Ctrl+Shift+E is SplitVertical. Reachable via palette and context menu.
- SendChars word/line motions (⌥←/→, ⌘←/→, ⌘⌫) — exist only because Mac keys differ; Linux/Windows shells
  handle Ctrl+←/→, Home/End natively. The test compares these by action *with* arguments or exempts
  SendChars wholesale (builder's call).
- SwitchToTab{position: 10} — desktop Alt+0; on macOS ⌘0 is ResetFontSize everywhere.
- FollowHyperlink / mouse rows — compared separately or not at all (builder's call); this pass is
  about keyboard chords.

## Declined presses

`declined` exists only on fallback rows (`FallbackMapping`); a written `InputBinding` always passes a
declined press on (`src/contour/input/KeyRouter.hpp`). So "every new row is `both`" has one exception:
a row that must consume a declined press stays `FallbackOnly`, as ⌘C, ⌘K and Ctrl+Shift+G already are.
In this pass that is ⇧⌘O only. Extending the written-row schema with a decline policy is out of scope.

## Parity test

A new Catch2 case in `src/contour/config/` (next to `DefaultBindings_test.cpp` /
`BindingCollisions_test.cpp`): for each defaults edition, take
`makeDefaultInputMappings(edition, Xkb)` plus the Xkb fallbacks, and the same for `Mac`; collect the set
of *actions* (by action name, ignoring arguments only where the exemption table says so) that have at
least one keyboard chord; assert the symmetric difference equals the exemption table exactly. An entry
in the exemption table that is no longer needed fails too, so the table cannot rot.

The existing `BindingCollisions` test must stay green: no new row may collide with an existing one on
any platform/edition.

## Menu bar and palette display

`command::shortcutIndex` (`src/contour/command/Shortcut.cpp`) picks one chord per command to display.
With two chords for OpenCommandPalette on macOS, it must pick `⌘P`. *Builder's call* how: table order
(the `⌘P` row first) is the minimal fix if the index already takes the first match; otherwise an
explicit "display chord" choice. Same for ClosePane on the desktop (show Ctrl+Shift+W or keep
Ctrl+Shift+X? — **builder's call, my suggestion: Ctrl+Shift+W**, the more familiar one).
`Shortcut_test.cpp` currently pins `⇧⌘P` and is updated accordingly.

## Golden files and docs

- `src/contour/config/testdata/default-bindings-*.txt` are regenerated and the diff reviewed by hand:
  every changed line must correspond to a row in this document.
- `docs/configuration/key-mapping.md` gains a "Default chords" table: command, Linux/Windows, macOS, and
  the reason for any departure. The existing sentence promising Ctrl+Shift+C becomes true.
- Release note entry (metainfo) listing the new chords per platform.

## Constraints

- Project guidelines (`AGENT.md`): data-driven tables, `enum class` over `bool`, zero warnings,
  clang-format. New code lands with tests.
- Fallback rows are consulted only when no explicit binding matches, so existing explicit bindings win.
- Do not touch `vendor/core-cpp`.

## Calls I made (not yet confirmed by the user)

- `⌘W` closes the focused pane (then the tab), `⇧⌘W` closes the tab. Played back to the user without
  objection, but not explicitly confirmed.
- The concrete new Mac chords: `⇧⌘O`, `⌃⌘.`, `⌃⌘O`, `⌃⌘S`, `⌃⌘,`, `⌥⇧⌘V`.
- Desktop: `Ctrl+Shift+W` as an *additional* ClosePane chord; CloseTab stays unbound there.
- Ctrl+Shift+C is a plain `both` row (no consume; it does not decline).
- PasteClipboard(strip) on macOS is ⌃⌘V.
- ⇧⌘O is fallback-only (consumes a declined press), so it is the one new chord a generated config does
  not list.
- Display chord: a user's own written binding wins. An existing Mac config that lists ⇧⌘P keeps showing
  ⇧⌘P in the menu (it works); only fresh configs show ⌘P. No new `ChordPreference` policy.
- ScrollMarkUp/Down need no exemption: Ctrl+Shift+↑/↓ is already bound on every platform.

## Left to the builder

- Whether the Ctrl+Alt ⇄ ⌃⌘ translation is an internal `ChordSpec` kind or explicit rows.
- How the display chord is chosen when a command has two.
- Exact wording of row comments and the docs table layout.
