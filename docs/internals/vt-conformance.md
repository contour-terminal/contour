# VT conformance testing

How Contour measures itself against the two established VT test programs, and — more importantly —
what each measurement is actually worth. This is a design document, not a status report: it describes
the harness and the discipline, not how many tests pass today. For the numbers, run the suite.

`src/vtconformance` drives **vttest** and **esctest** against a headless `vtbackend::Terminal`, with
no GUI, no window system and no renderer.

## The two suites judge different things

**esctest** is *unattended*: it drives the terminal and checks its own assertions, so it needs no
goldens and no screen sampling. It **gates the build**.

**vttest** is interactive by design — it draws a screen, describes in prose what a correct terminal
would have drawn, and waits for a human to look. A headless harness cannot read prose, so each screen
is captured as a **golden dump** and compared byte-for-byte against a blessed copy.

That difference is why gating is decided per *scenario* rather than per suite: within one suite the
oracles differ in how far they can be trusted.

## The barrier is the byte stream, not a clock

The load-bearing decision. vttest blocks on a prompt; the harness must know *when* to answer, and when
the screen it is about to capture is final.

**It answers on the marker, not on a timeout.** `tprintf`/`cprintf` end in `FLUSH` (`esc.c:7`), so
every prompt is on the wire the instant it is printed — the byte stream is the only channel reliably
fresh at block time. The log is block-buffered, and a screen cannot say *when* it became final.

Typing on the banner is provably safe rather than merely observed to work: `holdit()` is
`inflush(); tprintf("Push <RETURN>"); readnl();` (`unix_io.c:239-241`) — the flush runs *before* the
banner, at all 116 call sites.

**Only `holdit()` may capture a golden.** Its banner is the last thing on the wire before the read.
Every other prompt is an `instr()` with more output still behind it, so cutting the stream at that
marker freezes a half-drawn line.

**A clock has exactly one job here.** Deciding *when to answer* is causal. Deciding that a child will
*never speak again* cannot be — "blocked forever" is only observable by waiting. Those are separate
concerns and the driver keeps them separate: markers answer prompts, and a distinct `lastHeardFrom`
timer detects a wedge. Fusing them is what makes a driver sample screens on a deadline.

Two traps, both of which bit during development:

- Omitting the wedge detector turns an interactive chapter's fast skip into a 15-minute hang.
- Writing the wedge detector against `read()`'s `EAGAIN` breaks *every* chapter in milliseconds.
  **`EAGAIN` means "no data right now", not "silence"**, and fires before the child is even
  scheduled. Measure silence; never infer a duration from an errno.

## `*` runs every menu *item* — and an item need not be a test

vttest's `*` means "run every item in this menu". Reading it as "run every test" is wrong, and the
mistake is invisible: the scenario still passes, having measured nothing.

Three kinds of item are not tests:

- **A submenu.** `*` dispatches it, it prompts for its own menu input, and the driver walks straight
  back out. The sub-chapter needs a scenario of its own.
- **A cross-link.** An item that merely calls another chapter's menu.
- **A toggle**, or a `not_impl` stub. Chapter 11.6's item 1 is
  `{ txt_override_color, toggle_color_mode }` — captioned "Disable color-switching" *while colour is
  on*. `*` dispatched it first and the ISO-6429 **colour** chapter measured colour with colour
  switched off, for as long as the chapter existed.

**Decide by reading vttest's menu tables in its source, not by running it.** An item whose label is
*dynamic* (built with `sprintf`) is a toggle by construction. vttest's own `-l` log also narrates the
answer, one line per dispatch: `Note: choice 11.6.1: Disable color-switching`.

## Goldens: recorded is not reviewed

A golden dump is captured, then **blessed**. The distinction that matters:

- **Recorded** — captured, pins today's behaviour, *not yet judged correct*.
- **Reviewed** — judged against vttest's own source, the reference terminals under
  `$CONTOUR_VT_REFERENCE_SOURCES` (see [Reference sources](#reference-sources)), and the DEC manuals.

**Both gate.** "Is this screen correct?" and "should a change to it be visible?" are different
questions, and a recorded golden answers the second perfectly well. If a frozen screen later proves
wrong, the gate is what makes fixing it a deliberate, reviewed re-bless instead of silent drift.

Gating requires only that the capture be *reproducible*, which the causal barrier provides: the whole
suite report is byte-identical across runs. Verify that before adding a gating scenario.

**A golden captured at the wrong instant does not look wrong — it looks like a different, plausible
screen.** Several goldens froze driver bugs and read as perfectly reasonable terminal output.
Reviewing means checking a screen against vttest's stated claim, which is usually mechanical: *"There
should be no cells with the default foreground or background"* is a property you can count.

## Known gaps are ratcheted, not ignored

`src/vtconformance/test/known-gaps.txt` and `esctest-known-failures.txt` list what is known to fail,
each with its justification. The suite fails on a **new** gap, and also reports entries that no longer
reproduce so they can be removed. A gap must be a deliberate, documented decision — an intentional
divergence or an unimplemented sequence — never a shrug.

## Which goldens have been reviewed

Recorded is not reviewed, so which is which has to be written down or it is lost — a reviewed golden
is indistinguishable from a recorded one by inspection. This is that record. It is not a pass count;
the suite prints those.

Every chapter below is driven by the causal barrier, so nothing here is blocked on the driver any
more: what is left is the *judging*, and it is per chapter and per screen. All 143 goldens across the
18 visual scenarios have been reviewed.

| Chapter | Goldens | What the review turned on |
|---|---|---|
| 01 cursor movements | 6 | Each screen states its own verdict. The E-frame is centred to the exact half-cell (rows 9–16, cols 11–70) at both 80 and 132 columns. |
| 02 screen features | 15 | Judged from each screen's own caption, *not* by counting `holdit()` call sites — the column-mode test holds inside a loop, so one call site is four runtime holds. step09 had frozen a driver bug. |
| 04 double-sized | 6 | `decstbm(8,24)` then 12×`ri()` leaves "exactly half of the box", and does: five lines at 20–24, the last a `DoubleHeightTop` whose partner fell off the region edge. |
| 07 VT52 mode | 3 | `tst_vt52` has exactly three holds. Two files on disk had been captured at a non-hold and were deleted. |
| 08 VT102 features | 14 | *"The right column should be staggered by one"* — the letter runs' right edges step by exactly −1 down all 24 rows. A first pass misread a −2; that was the measurement, not the screen. |
| 09 known bugs | 28 | The chapter's point is that a modern terminal has none of them, and Contour has none. `bug_e` is the sharpest: a real VT100 clamps to column ≤66 on a double-wide line; Contour puts the X at exactly 100. |
| 11.5 ISO-6429 cursor | 9 | The same box drawn five ways, so they check each other. VPA's is 41 wide and **right** to be: `tst_VPA` walks its bottom edge with `print_str("\b*\b")`, which steps left before drawing. |
| 11.6 ISO-6429 colours | 57 | Rebuilt from 15 after the toggle bug. 33 of 11.6.6's 35 text planes are byte-identical to an already-reviewed screen, so that review transfers; the 2 that differ do so by one line of prose. |
| 11.7 ISO-6429 other | 6 | REP is a real conformance judgement and Contour takes the standard side: vttest allows 11 `+`s as undefined behaviour, the golden shows 2. |

**Check a screen against the claim it makes, never against a rule generalised from its neighbour.**
11.6.4/11.6.5 state *"no cells with the default foreground or background"* in words; 11.6.6 never
claims it and correctly has default cells.

Chapters 03, 05, 11.1, 11.3.4 and 11.8 have no goldens: they are skipped, and a golden of a
half-driven screen is worse than none.

## What is left, and why

The gap files say which sequences are unimplemented. This is for the decisions that outlive them.

- **Bidirectional text is a toggle, and the toggle is a no-op.** `RightToLeftMode` (DECRLM, 34),
  `HebrewEncodingMode` (DECHEM, 36) and `RightToLeftCopyMode` (DECRLCM, 96) are settable and reported
  honestly by DECRQM — SM/RM toggle them, DECRQM answers Set/Reset — but **nothing reads the bit**.
  They are the entry points for real right-to-left and Hebrew support, which is a wanted feature
  rather than a checkbox, and they are the priority of their group. The rendering half of the problem
  is a separate and larger one; see `docs/internals/text-stack.md`, which does not address BiDi
  either. They are part of a block of seventeen VT525 keyboard, national and hardware modes in
  `primitives.h`'s `DECMode` that are honest toggles today, each carrying its own `TODO`. Reporting a
  mode's state truthfully while not acting on it is deliberate: it is a step above the
  `PermanentlyReset` modes, which can never mean anything here.
- **The `*` table has not been audited against vttest's menu tables.** The rule above — `*` cannot
  cross a submenu — was applied where it was found (11.7's protected-area item, 11.2.5's UPSS item),
  not proven across the table. Every remaining `*` is a claim that none of its items is a submenu, a
  cross-link or a toggle, and that claim is unverified. Both known instances were found by *reading*
  vttest's source, never by a failing run: that is the only way this class shows up.
- **Two vttest fixes are worth sending upstream.** Its `-l` log is opened without a buffering mode, so a
  killed vttest leaves a transcript short of the verdicts it produced — and a short transcript reports
  fewer failures, which reads as success. (The runner detects that already, via vttest's parting
  `That's all, folks!`; the fix removes the failure mode rather than catching it.) And its DECCRA
  carries a trailing `;` (`esc.c:732`), which ECMA-48 makes a ninth, empty parameter — legal, but not
  what DECCRA is, and it is what hid a real Contour bug for as long as the chapter never ran.
- **The screen dump's legend alphabet runs out at 62 renditions.** `ScreenDump.cpp` hands out
  `A-Za-z0-9`; 11.6.2's colour test-pattern has 128 renditions, so 66 collapse onto `?`. It degrades
  gracefully — the legend is itself part of the compared dump and lists renditions in first-appearance
  order, so a changed rendition *set*, and any swap that moves a first appearance, are both still
  caught. The dump is blind only to a swap between two repeat occurrences of two overflow renditions.
  Widening the alphabet past ASCII makes the attribute plane multi-byte, which is real cost against a
  narrow gap.
- **There is no VT-semantics coverage ratchet.** `allFunctions()` is already a machine-readable
  registry of every implemented sequence with its conformance level; it could be the denominator, with
  a data-driven corpus as the numerator and a build failure when a sequence lands without a test. Key
  it on `Function::id()`, **not** on the mnemonic — mnemonics are not unique. `ANSIDSR` (`CSI n`) and
  `DSR` (`CSI ? n`) both report `mnemonic == "DSR"`.
- **DECRQSS is not gated on the operating level.** After a VT52 round-trip drops the terminal to
  VT100, a real VT100 does not recognise DECRQSS (a VT300+ request) and stays silent; Contour still
  answers. vttest tolerates either and answering is harmless.
- **DECCOLM while maximized does not un-maximize.** The grid is authoritative and renders correctly
  inside the pinned frame, so this is a UX preference rather than a rendering bug. Resizing after
  `showNormal()` — even deferred — commits a surface geometry that does not match the still-maximized
  configure, which is a *fatal* xdg-shell protocol error. Refusing is what foot and kitty do too. It
  needs a proper unmaximize/ack/resize handshake, and is arguably X11-only.

## Diagnose by the failure *set*, not the count

Repeatedly, a large number of failures has turned out to be one defect. Count *desyncs* and look at
*which* tests fail together; the shape of the set names the cause, and the count does not.

## The shape to watch for

Most defects found in this area were not missing checks. They were checks that ran, reported green,
and whose structure guaranteed they could not see the thing they named: a verdict oracle that read one
of the two record types its program emits, and then another that read the record but dropped the
verdict inside it; a gating flag that only judged scenarios already gating; `*` on a submenu; a
hand-copied list of cell flags that never learnt about a new one, leaving the protected-area goldens
unable to tell a protected cell from an unprotected one.

When a conformance check passes, ask what it would take for it to fail. If there is no such input, it
is measuring nothing. **Answer that by measuring, not by reading**: break the thing on purpose and
watch the check go red. A scenario driving DECRQUPSS passed just as happily against a terminal
answering deliberate nonsense, because the one verdict vttest emitted was a string the oracle did not
match — and nothing about the passing run said so.

## Reference sources

Cross-check any sequence's semantics against **both** the established terminal source trees **and** a
primary DEC manual — reading the source alone is not enough, and scattered web summaries are not
sources. xterm's `ctlseqs.txt` is the canonical sequence catalog; DEC STD 070 and the VT520/VT525
Programmer Information manual are the standards. Note where terminals *diverge or punt* — that is
usually where the interesting decision is. **xterm is the only reference that must be measured rather
than read** (run it under Xvfb): for some sequences its manual, its source and its actual output
disagree.

### Where the source trees live

The trees are **not** vendored into this repository — they are large third-party checkouts. Clone the
ones you need from their upstreams and point `$CONTOUR_VT_REFERENCE_SOURCES` at the directory that
holds them; tooling and the guidance in `AGENT.md` resolve every tree relative to that variable, so
no personal path is ever committed. Any layout works as long as each tree keeps the subdirectory name
in the first column:

| Subdir              | Upstream                                             |
| ------------------- | ---------------------------------------------------- |
| `xterm`             | <https://invisible-island.net/xterm/> (or the `xterm-snapshots` tarballs / <https://github.com/ThomasDickey/xterm-snapshots>) |
| `xterm.js`          | <https://github.com/xtermjs/xterm.js>                |
| `windows-terminal`  | <https://github.com/microsoft/terminal>              |
| `kitty`             | <https://github.com/kovidgoyal/kitty>                |
| `konsole`           | <https://invent.kde.org/utilities/konsole>           |
| `foot`              | <https://codeberg.org/dnkl/foot>                     |
| `ghostty`           | <https://github.com/ghostty-org/ghostty>             |
| `wezterm`           | <https://github.com/wez/wezterm>                     |
| `mintty`            | <https://github.com/mintty/mintty>                   |

Set the variable however your environment prefers. For a Claude Code checkout, the least intrusive
place is the (git-ignored) `.claude/settings.local.json`, which injects it into the tool sandbox:

```json
{
  "env": { "CONTOUR_VT_REFERENCE_SOURCES": "/absolute/path/to/your/terminals" }
}
```

Then, e.g. `grep -rniE "DECATC" "$CONTOUR_VT_REFERENCE_SOURCES/xterm/"`.
