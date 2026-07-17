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
- **Reviewed** — judged against vttest's own source, the reference terminals in
  `~/usr/src/terminals/`, and the DEC manuals.

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

## Diagnose by the failure *set*, not the count

Repeatedly, a large number of failures has turned out to be one defect. Count *desyncs* and look at
*which* tests fail together; the shape of the set names the cause, and the count does not.

## The shape to watch for

Most defects found in this area were not missing checks. They were checks that ran, reported green,
and whose structure guaranteed they could not see the thing they named: a verdict oracle that read one
of the two forms its program emits; a gating flag that only judged scenarios already gating; `*` on a
submenu; a hand-copied list of cell flags that never learnt about a new one, leaving the
protected-area goldens unable to tell a protected cell from an unprotected one.

When a conformance check passes, ask what it would take for it to fail. If there is no such input, it
is measuring nothing.

## Reference sources

Cross-check any sequence's semantics against **both** the terminal source trees under
`~/usr/src/terminals/` **and** a primary DEC manual — reading the source alone is not enough, and
scattered web summaries are not sources. xterm's `ctlseqs.txt` is the canonical sequence catalog;
DEC STD 070 and the VT520/VT525 Programmer Information manual are the standards. Note where terminals
*diverge or punt* — that is usually where the interesting decision is. **xterm is the only reference
that must be measured rather than read** (run it under Xvfb): for some sequences its manual, its
source and its actual output disagree.
