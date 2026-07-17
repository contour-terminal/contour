// SPDX-License-Identifier: Apache-2.0
#include <array>
#include <string>
#include <string_view>

#include <vtconformance/Suite.h>

using namespace std::string_view_literals;
using vtbackend::VTType;
using vtpty::ColumnCount;
using vtpty::LineCount;
using vtpty::PageSize;

namespace vtconformance
{

namespace
{
    // Every prompt vttest blocks on, taken from its own strings, and how to answer it.
    //
    // Order matters only in that the first match wins; the patterns are disjoint in practice.
    constexpr auto Prompts = std::array {
        Prompt { .pattern = "Enter choice number"sv, .action = PromptAction::Menu },
        Prompt { .pattern = "Bad choice, try again"sv, .action = PromptAction::Menu },
        // The only real hold: holdit() is `inflush(); tprintf("Push <RETURN>"); readnl()`
        // (unix_io.c:239-241), so the banner is the LAST thing on the wire before the read. That is
        // what makes it safe to cut the byte stream here -- everything before it is the screen a human
        // would have been shown, and nothing after it belongs to that screen.
        Prompt { .pattern = "Push <RETURN>"sv, .action = PromptAction::Hold },

        // Reads that are NOT holdit(), and so must not capture a golden.
        //
        // Each of these is an instr() with more output still to come behind it -- a newline, another
        // line of prose, the query itself -- so the screen is nowhere near final at the marker, and a
        // dump taken there would freeze a half-drawn line. They only need answering.
        //
        // The LNM test spells its prompt out longhand and then reads through instr() (reports.c:597).
        // Answered as a KEY, because what the RETURN key sends is the entire question: LNM set demands
        // CR LF, LNM reset demands CR alone (reports.c:604,617). Typing "\n" -- which is what the
        // driver did for as long as this chapter existed -- matches neither, so both halves reported
        // "-- Not expected" in every run on record.
        Prompt {
            .pattern = "Push the RETURN key"sv, .action = PromptAction::Key, .key = vtbackend::Key::Enter },
        // The operating-level check after leaving VT52 blocks on a manual RETURN precisely *because*
        // a correct VT100-level terminal ignores the DECSCL query it just sent (its own words: "You
        // should have to press return to continue"). Answering it is how the harness confirms the
        // level actually dropped, rather than wedging on the very behaviour VT52 mode makes correct.
        // vt52.c:225-228 prints the prose, THEN sends DECRQSS, THEN reads -- so the marker is three
        // writes early.
        // Same shape as LNM, one chapter over: DECBKM decides whether the backspace key sends BS or
        // DEL (InputGenerator.cpp:83-90), so only a key press can answer it. Before this row, 11.3.4
        // "passed" by letting vttest's own alarm(60) give up on the prompt -- green because nothing
        // was measured.
        Prompt { .pattern = "Press the backspace key"sv,
                 .action = PromptAction::Key,
                 .key = vtbackend::Key::Backspace },
        Prompt { .pattern = "press return to continue"sv, .action = PromptAction::Type, .input = "\n"sv },
        Prompt { .pattern = "Enter 0 to exit"sv, .action = PromptAction::Type, .input = "0\n"sv },

        // Sub-tests that ask for real key presses, answered with each test's own documented escape.
        //
        // These are a safety net, not a way of exercising those tests: a chapter whose items include
        // one is marked Interactive and skipped outright, because a headless engine has no keyboard
        // and pretending otherwise would report untested ground as green. What these rows buy is that
        // if such a prompt is ever reached anyway, the driver walks out of it rather than wedging.
        //
        // A prompt the program does NOT accept would be re-drawn and re-answered forever, so the
        // driver also carries a step budget (TerminalHarness::Options::maximumSteps).
        Prompt { .pattern = "Repeat a key to quit"sv, .action = PromptAction::Type, .input = "aa"sv },
        Prompt { .pattern = "press any key twice to quit"sv, .action = PromptAction::Type, .input = "aa"sv },
        Prompt { .pattern = "Finish with TAB"sv, .action = PromptAction::Type, .input = "\t"sv },
        Prompt { .pattern = "Finish with RETURN"sv, .action = PromptAction::Type, .input = "\n"sv },
        Prompt { .pattern = "Press 'q' to quit"sv, .action = PromptAction::Type, .input = "q"sv },
    };

    // vttest's menu paths. Each scenario walks into a chapter, runs every test in it with `*`, and
    // walks back out with `0`. The trailing `0` at top level quits vttest.
    //
    // `*` cannot be used on chapter 11: its items are themselves submenus that would block waiting
    // for their own menu input. Its sub-chapters therefore get scenarios of their own.

    // Chapters 1, 2, 7 and 8 have no submenu: selecting them runs every test in the chapter directly,
    // so a `*` here would fall through to the MAIN menu and re-run the entire suite (blowing each
    // scenario up into a full run, and drawing later chapters' output into this scenario's goldens).
    // They walk in with the chapter number and straight back out with `0`.
    constexpr auto CursorMovementKeys = std::array { "1"sv, "0"sv };
    constexpr auto ScreenFeatureKeys = std::array { "2"sv, "0"sv };
    constexpr auto CharacterSetKeys = std::array { "3"sv, "*"sv, "0"sv, "0"sv };
    constexpr auto DoubleSizedKeys = std::array { "4"sv, "0"sv };
    constexpr auto KeyboardKeys = std::array { "5"sv, "0"sv, "0"sv };
    // Chapters 6, 9 and 10 do present a submenu, so `*` correctly runs their items and stays put.
    constexpr auto ReportKeys = std::array { "6"sv, "*"sv, "0"sv, "0"sv };
    constexpr auto Vt52Keys = std::array { "7"sv, "0"sv };
    constexpr auto Vt102Keys = std::array { "8"sv, "0"sv };
    constexpr auto KnownBugKeys = std::array { "9"sv, "*"sv, "0"sv, "0"sv };
    constexpr auto ResetKeys = std::array { "10"sv, "*"sv, "0"sv, "0"sv };

    // Chapter 11's sub-chapters are submenus of submenus, so a scenario has to walk all the way in and
    // all the way back out: `11`, the sub-chapter, the item, `*` to run the item's own tests, then one
    // `0` per level -- four of them, for the item's menu, the sub-chapter's, chapter 11's, and vttest's.
    //
    // Which items are worth a scenario is decided by vttest's own menu tables, not by counting:
    //   * Item 1 of each is a CROSS-LINK to the level below (VT520's "Test VT420 features" is
    //     `tst_vt420`, vt520.c:721; VT420's is `tst_vt320`, vt420.c:2892; VT320's is `tst_vt220`,
    //     vt320.c:1674). A scenario for one would just re-run the chapter below it.
    //   * Three items are `not_impl` -- vttest's own stub, not a test: VT420's macro-definition
    //     (vt420.c:2896) and VT520's editing and keyboard-control (vt520.c:723-724).
    // That leaves fourteen of the twenty that test anything at all.
    constexpr auto Vt320CursorKeys = std::array { "11"sv, "2"sv, "2"sv, "*"sv, "0"sv, "0"sv, "0"sv, "0"sv };
    constexpr auto Vt320PageFormatKeys =
        std::array { "11"sv, "2"sv, "3"sv, "*"sv, "0"sv, "0"sv, "0"sv, "0"sv };
    constexpr auto Vt320PageMovementKeys =
        std::array { "11"sv, "2"sv, "4"sv, "*"sv, "0"sv, "0"sv, "0"sv, "0"sv };
    constexpr auto Vt320ReportKeys = std::array { "11"sv, "2"sv, "5"sv, "*"sv, "0"sv, "0"sv, "0"sv, "0"sv };
    constexpr auto Vt320ScreenKeys = std::array { "11"sv, "2"sv, "6"sv, "*"sv, "0"sv, "0"sv, "0"sv, "0"sv };

    constexpr auto Vt420CursorKeys = std::array { "11"sv, "3"sv, "2"sv, "*"sv, "0"sv, "0"sv, "0"sv, "0"sv };
    constexpr auto Vt420EditingKeys = std::array { "11"sv, "3"sv, "3"sv, "*"sv, "0"sv, "0"sv, "0"sv, "0"sv };
    constexpr auto Vt420KeyboardKeys = std::array { "11"sv, "3"sv, "4"sv, "*"sv, "0"sv, "0"sv, "0"sv, "0"sv };
    constexpr auto Vt420RectangularKeys =
        std::array { "11"sv, "3"sv, "6"sv, "*"sv, "0"sv, "0"sv, "0"sv, "0"sv };
    constexpr auto Vt420ReportKeys = std::array { "11"sv, "3"sv, "7"sv, "*"sv, "0"sv, "0"sv, "0"sv, "0"sv };
    constexpr auto Vt420ScreenKeys = std::array { "11"sv, "3"sv, "8"sv, "*"sv, "0"sv, "0"sv, "0"sv, "0"sv };

    // 11.7's Protected-Area item is a submenu too (nonvt100.c:631-634 -> tst_SPA), so chapter 11's
    // rule bites here as well: `*` dispatches it, it prompts, and the driver walks back out. The rule
    // is not "chapter 11 is special", it is "`*` cannot cross a submenu".
    constexpr auto Iso6429ProtectedKeys =
        std::array { "11"sv, "7"sv, "1"sv, "*"sv, "0"sv, "0"sv, "0"sv, "0"sv };

    // ... and 11.2.5's User-Preferred Supplemental Set item is the third (tst_upss, charsets.c:986).
    // Vt320ReportKeys' `*` dispatches it, it prompts for its own menu input, the driver answers `0`,
    // and it walks straight back out -- running only the `reset_upss()` on its way out (charsets.c:1015)
    // and never reaching tst_DECRQUPSS. So this item needs a scenario of its own.
    //
    // It does NOT get a `*`, and that is the point of the rule rather than an exception to it: item 1
    // of the UPSS submenu is `tst_characters`, which waits on real key presses for its soft-character
    // item, so a `*` here would wall up before reaching anything. The DECRQUPSS item is addressed
    // directly instead: 11 -> 2 (VT320) -> 5 (Reports) -> 5 (UPSS) -> 4 (Test DECRQUPSS), then one `0`
    // per menu level to unwind -- UPSS, Reports, VT320, chapter 11, main.
    constexpr auto Vt320UpssKeys =
        std::array { "11"sv, "2"sv, "5"sv, "5"sv, "4"sv, "0"sv, "0"sv, "0"sv, "0"sv, "0"sv };

    constexpr auto Vt520CursorKeys = std::array { "11"sv, "4"sv, "2"sv, "*"sv, "0"sv, "0"sv, "0"sv, "0"sv };
    constexpr auto Vt520ReportKeys = std::array { "11"sv, "4"sv, "5"sv, "*"sv, "0"sv, "0"sv, "0"sv, "0"sv };
    constexpr auto Vt520ScreenKeys = std::array { "11"sv, "4"sv, "6"sv, "*"sv, "0"sv, "0"sv, "0"sv, "0"sv };

    constexpr auto Vt220Keys = std::array { "11"sv, "1"sv, "*"sv, "0"sv, "0"sv, "0"sv };
    constexpr auto Vt320Keys = std::array { "11"sv, "2"sv, "*"sv, "0"sv, "0"sv, "0"sv };
    constexpr auto Vt420Keys = std::array { "11"sv, "3"sv, "*"sv, "0"sv, "0"sv, "0"sv };
    constexpr auto Vt520Keys = std::array { "11"sv, "4"sv, "*"sv, "0"sv, "0"sv, "0"sv };
    constexpr auto Iso6429CursorKeys = std::array { "11"sv, "5"sv, "*"sv, "0"sv, "0"sv, "0"sv };
    // Chapter 11.6 cannot use `*` either, and for a reason worth spelling out: its item 1 is not a test
    // at all but a TOGGLE -- `{ txt_override_color, toggle_color_mode }` (color.c:697), captioned
    // "Disable color-switching" while colours are on. `tst_colors` deliberately forces `do_colors = TRUE`
    // on entry (color.c:710-712), and `*` then hands that toggle back off before a single test runs;
    // vttest's own log narrated it as `choice 11.6.1: Disable color-switching`. Everything after it drew
    // with `c_sgr()` silently dropping its `;33;44` suffix (color.c:59-61), so the ISO-6429 *colour*
    // chapter measured colours with colours switched off -- and all fifteen of its goldens recorded the
    // colourless screens as correct.
    //
    // Items 2..9 are the real tests, and each gets a scenario. Only item 6 opens a submenu of its own
    // (`test_vt100_colors`), and it forces `do_colors = TRUE` for that submenu as well, so `*` is safe
    // exactly there.
    constexpr auto Iso6429ColorPatternKeys = std::array { "11"sv, "6"sv, "2"sv, "0"sv, "0"sv, "0"sv };
    constexpr auto Iso6429ColorSgr0Keys = std::array { "11"sv, "6"sv, "3"sv, "0"sv, "0"sv, "0"sv };
    constexpr auto Iso6429ColorBceEraseKeys = std::array { "11"sv, "6"sv, "4"sv, "0"sv, "0"sv, "0"sv };
    constexpr auto Iso6429ColorBceEchKeys = std::array { "11"sv, "6"sv, "5"sv, "0"sv, "0"sv, "0"sv };
    constexpr auto Iso6429ColorVt102Keys =
        std::array { "11"sv, "6"sv, "6"sv, "*"sv, "0"sv, "0"sv, "0"sv, "0"sv };
    // Item 7 is a CROSS-LINK, the same shape as chapter 11's: `test_ecma48_misc` (color.c:558-565) only
    // turns colours on and calls `tst_ecma48_misc` -- chapter 11.7's own menu. So it needs `*` like any
    // submenu, and its Protected-Area item needs a scenario of its own for the same reason 11.7.1 does.
    // What these two buy over 11.7 is the BCE half of the question: an erase inside a protected area, or
    // a scroll, has to leave the *current* background behind, and 11.7 runs them with no colour at all.
    constexpr auto Iso6429ColorEcma48Keys =
        std::array { "11"sv, "6"sv, "7"sv, "*"sv, "0"sv, "0"sv, "0"sv, "0"sv };
    constexpr auto Iso6429ColorProtectedKeys =
        std::array { "11"sv, "6"sv, "7"sv, "1"sv, "*"sv, "0"sv, "0"sv, "0"sv, "0"sv, "0"sv };
    constexpr auto Iso6429ColorScreenKeys = std::array { "11"sv, "6"sv, "8"sv, "0"sv, "0"sv, "0"sv };
    constexpr auto Iso6429ColorSgr2227Keys = std::array { "11"sv, "6"sv, "9"sv, "0"sv, "0"sv, "0"sv };
    constexpr auto Iso6429OtherKeys = std::array { "11"sv, "7"sv, "*"sv, "0"sv, "0"sv, "0"sv };
    constexpr auto XtermKeys = std::array { "11"sv, "8"sv, "*"sv, "0"sv, "0"sv, "0"sv };

    constexpr auto Scenarios = std::array {
        Scenario { .id = "vttest.01.cursor-movements",
                   .title = "Test of cursor movements",
                   .keys = CursorMovementKeys,
                   .kind = ScenarioKind::Visual,
                   .minimumLevel = VTType::VT100,
                   .driveMode = DriveMode::Live,
                   .gatesBuild = true },
        Scenario { .id = "vttest.02.screen-features",
                   .title = "Test of screen features",
                   .keys = ScreenFeatureKeys,
                   .kind = ScenarioKind::Visual,
                   .minimumLevel = VTType::VT100,
                   .driveMode = DriveMode::Live,
                   .gatesBuild = true },
        // Chapter 3 runs every item with `*`, and one of those items -- the soft-character test --
        // asks the user to press keys and watch them render in the downloaded font. A headless engine
        // has no keyboard, so the chapter cannot be driven as a whole today. Unlocking its other
        // items (VT100 charsets, VT220 locking shifts, single shifts) needs per-item scenarios rather
        // than `*` -- @see vttest.11.2.5.5 and vttest.11.7.1 for that shape. Reported as skipped,
        // never as passed.
        Scenario { .id = "vttest.03.character-sets",
                   .title = "Test of character sets",
                   .keys = CharacterSetKeys,
                   .kind = ScenarioKind::Interactive,
                   .minimumLevel = VTType::VT100 },
        Scenario { .id = "vttest.04.double-sized",
                   .title = "Test of double-sized characters",
                   .keys = DoubleSizedKeys,
                   .kind = ScenarioKind::Visual,
                   .minimumLevel = VTType::VT100,
                   .driveMode = DriveMode::Live,
                   .gatesBuild = true },
        // The keyboard chapter asks a human to press physical keys; a headless engine has none.
        // It is enumerated (rather than omitted) so the report can say "skipped", not "passed".
        Scenario { .id = "vttest.05.keyboard",
                   .title = "Test of keyboard",
                   .keys = KeyboardKeys,
                   .kind = ScenarioKind::Interactive,
                   .minimumLevel = VTType::VT100 },
        // Gates. Twenty-two of its tests check themselves and say so in the transcript, which is the
        // strongest oracle in the vttest half of this harness: no goldens, no screens, no judgement.
        //
        // Its LNM sub-test reads the RETURN through `instr()` (reports.c:598,612), which calls
        // pause_replay() (unix_io.c:134) -- so the read is live BY CONSTRUCTION and a command file
        // could never answer it: skip_to_tag deliberately skips any `Read:` inside a `Wait:`/`Done:`
        // span (replay.c:83-99). The driver has to. It now does so as a KEY PRESS, because what the
        // RETURN key sends under LNM is the whole question. @see PromptAction::Key.
        Scenario { .id = "vttest.06.terminal-reports",
                   .title = "Test of terminal reports",
                   .keys = ReportKeys,
                   .kind = ScenarioKind::SelfChecking,
                   .minimumLevel = VTType::VT100,
                   .driveMode = DriveMode::Live,
                   .gatesBuild = true },
        Scenario { .id = "vttest.07.vt52-mode",
                   .title = "Test of VT52 mode",
                   .keys = Vt52Keys,
                   .kind = ScenarioKind::Visual,
                   .minimumLevel = VTType::VT100,
                   .driveMode = DriveMode::Live,
                   .gatesBuild = true },
        Scenario { .id = "vttest.08.vt102-features",
                   .title = "Test of VT102 features (Insert/Delete Char/Line)",
                   .keys = Vt102Keys,
                   .kind = ScenarioKind::Visual,
                   .minimumLevel = VTType::VT100,
                   .driveMode = DriveMode::Live,
                   .gatesBuild = true },
        Scenario { .id = "vttest.09.known-bugs",
                   .title = "Test of known bugs",
                   .keys = KnownBugKeys,
                   .kind = ScenarioKind::Visual,
                   .minimumLevel = VTType::VT100,
                   .driveMode = DriveMode::Live,
                   .gatesBuild = true },
        Scenario { .id = "vttest.10.reset-and-self-test",
                   .title = "Test of reset and self-test",
                   .keys = ResetKeys,
                   .kind = ScenarioKind::SelfChecking,
                   .minimumLevel = VTType::VT100,
                   .driveMode = DriveMode::Replay,
                   .gatesBuild = true },
        // Contains a keyboard-status sub-test that waits on real key presses. Same story as
        // chapter 3: unlocking the rest needs per-item scenarios instead of `*`.
        Scenario { .id = "vttest.11.1.vt220",
                   .title = "Test of VT220 features",
                   .keys = Vt220Keys,
                   .kind = ScenarioKind::Interactive,
                   .minimumLevel = VTType::VT220 },
        Scenario { .id = "vttest.11.2.vt320",
                   .title = "Test of VT320 features",
                   .keys = Vt320Keys,
                   .kind = ScenarioKind::SelfChecking,
                   .minimumLevel = VTType::VT320,
                   .driveMode = DriveMode::Replay,
                   .gatesBuild = true },
        Scenario { .id = "vttest.11.3.vt420",
                   .title = "Test of VT420 features",
                   .keys = Vt420Keys,
                   .kind = ScenarioKind::SelfChecking,
                   .minimumLevel = VTType::VT420,
                   .driveMode = DriveMode::Replay,
                   .gatesBuild = true },
        // Chapter 11's sub-chapters, one scenario each -- which is what the comment above the key
        // tables has always said they need, and never had. Without them `*` dispatches each item, the
        // item prompts for its own menu input, and the driver (out of keys) answers `0` and walks
        // straight back out: seventeen of chapter 11's twenty sub-chapters tested nothing while their
        // parents gated the build. The first one added, 11.3.6, found DECCRA broken on its first run.
        //
        // These are SelfChecking: each prints its own verdicts, which the transcript carries. Most do
        // not gate yet; they are new ground and their failures want reading before they break anyone's
        // build. That is a statement about how well their verdicts are understood, not about how
        // reliably they run -- so a scenario graduates to gating once its verdicts have been read, one
        // at a time. 11.2.5.5 is the first to do so.
        Scenario { .id = "vttest.11.2.2.vt320-cursor",
                   .title = "Test of VT320 cursor-movement",
                   .keys = Vt320CursorKeys,
                   .kind = ScenarioKind::SelfChecking,
                   .minimumLevel = VTType::VT320,
                   .driveMode = DriveMode::Live },
        Scenario { .id = "vttest.11.2.3.vt320-page-format",
                   .title = "Test of VT320 page-format controls",
                   .keys = Vt320PageFormatKeys,
                   .kind = ScenarioKind::SelfChecking,
                   .minimumLevel = VTType::VT320,
                   .driveMode = DriveMode::Live },
        Scenario { .id = "vttest.11.2.4.vt320-page-movement",
                   .title = "Test of VT320 page-movement controls",
                   .keys = Vt320PageMovementKeys,
                   .kind = ScenarioKind::SelfChecking,
                   .minimumLevel = VTType::VT320,
                   .driveMode = DriveMode::Live },
        Scenario { .id = "vttest.11.2.5.vt320-reports",
                   .title = "Test of VT320 reporting functions",
                   .keys = Vt320ReportKeys,
                   .kind = ScenarioKind::SelfChecking,
                   .minimumLevel = VTType::VT320,
                   .driveMode = DriveMode::Live },
        // The DECRQUPSS item Vt320ReportKeys' `*` cannot reach. @see Vt320UpssKeys.
        //
        // It gates, unlike its siblings above, because the reason they do not does not apply: it drives
        // exactly one test, which prints exactly one verdict, and that verdict has been read. It names
        // the set -- "DEC Supplemental Graphic (94 characters)" -- which vttest prints only after
        // parsing our reply and matching the designator against its own charset table at the size our
        // Ps claimed (parse_upss_name, charsets.c:751). A terminal that stayed silent, answered a
        // malformed DCS, or reported a set it was never assigned gets "unknown" instead, and this fails.
        // Checked by breaking the reply on purpose and watching it go red, which is the only way to
        // know a green check means anything.
        Scenario { .id = "vttest.11.2.5.5.vt320-upss",
                   .title = "Test of the User-Preferred Supplemental Set (DECRQUPSS)",
                   .keys = Vt320UpssKeys,
                   .kind = ScenarioKind::SelfChecking,
                   .minimumLevel = VTType::VT320,
                   .driveMode = DriveMode::Live,
                   .gatesBuild = true },
        Scenario { .id = "vttest.11.2.6.vt320-screen",
                   .title = "Test of VT320 screen-display functions",
                   .keys = Vt320ScreenKeys,
                   .kind = ScenarioKind::SelfChecking,
                   .minimumLevel = VTType::VT320,
                   .driveMode = DriveMode::Live },
        Scenario { .id = "vttest.11.3.2.vt420-cursor",
                   .title = "Test of VT420 cursor-movement",
                   .keys = Vt420CursorKeys,
                   .kind = ScenarioKind::SelfChecking,
                   .minimumLevel = VTType::VT420,
                   .driveMode = DriveMode::Live },
        Scenario { .id = "vttest.11.3.3.vt420-editing",
                   .title = "Test of VT420 editing sequences",
                   .keys = Vt420EditingKeys,
                   .kind = ScenarioKind::SelfChecking,
                   .minimumLevel = VTType::VT420,
                   .driveMode = DriveMode::Live },
        // Interactive: its DECNKM item asks for keypad keys ("Press one or more keys on the keypad")
        // and a headless engine has none. It looked SelfChecking until the driver stopped waiting out
        // silences -- it had been "passing" by letting vttest's own alarm(60) give up on the prompt,
        // which is green because nothing was measured, not because anything worked.
        //
        // Its DECBKM item IS answerable and worth a scenario of its own: what the backspace key sends
        // is a property of the terminal (InputGenerator.cpp:83-90), which is why the prompt table
        // answers it as a KEY. That behaviour is pinned meanwhile by TerminalEngine_test.
        Scenario { .id = "vttest.11.3.4.vt420-keyboard",
                   .title = "Test of VT420 keyboard-control",
                   .keys = Vt420KeyboardKeys,
                   .kind = ScenarioKind::Interactive,
                   .minimumLevel = VTType::VT420,
                   .driveMode = DriveMode::Live },
        Scenario { .id = "vttest.11.3.6.rectangular-area",
                   .title = "Test of VT420 rectangular area functions",
                   .keys = Vt420RectangularKeys,
                   .kind = ScenarioKind::SelfChecking,
                   .minimumLevel = VTType::VT420,
                   .driveMode = DriveMode::Live },
        Scenario { .id = "vttest.11.3.7.vt420-reports",
                   .title = "Test of VT420 reporting functions",
                   .keys = Vt420ReportKeys,
                   .kind = ScenarioKind::SelfChecking,
                   .minimumLevel = VTType::VT420,
                   .driveMode = DriveMode::Live },
        Scenario { .id = "vttest.11.3.8.vt420-screen",
                   .title = "Test of VT420 screen-display functions",
                   .keys = Vt420ScreenKeys,
                   .kind = ScenarioKind::SelfChecking,
                   .minimumLevel = VTType::VT420,
                   .driveMode = DriveMode::Live },
        Scenario { .id = "vttest.11.4.2.vt520-cursor",
                   .title = "Test of VT520 cursor-movement",
                   .keys = Vt520CursorKeys,
                   .kind = ScenarioKind::SelfChecking,
                   .minimumLevel = VTType::VT520,
                   .driveMode = DriveMode::Live },
        Scenario { .id = "vttest.11.4.5.vt520-reports",
                   .title = "Test of VT520 reporting functions",
                   .keys = Vt520ReportKeys,
                   .kind = ScenarioKind::SelfChecking,
                   .minimumLevel = VTType::VT520,
                   .driveMode = DriveMode::Live },
        Scenario { .id = "vttest.11.4.6.vt520-screen",
                   .title = "Test of VT520 screen-display functions",
                   .keys = Vt520ScreenKeys,
                   .kind = ScenarioKind::SelfChecking,
                   .minimumLevel = VTType::VT520,
                   .driveMode = DriveMode::Live },
        Scenario { .id = "vttest.11.4.vt520",
                   .title = "Test of VT520 features",
                   .keys = Vt520Keys,
                   .kind = ScenarioKind::SelfChecking,
                   .minimumLevel = VTType::VT520,
                   .driveMode = DriveMode::Replay,
                   .gatesBuild = true },
        Scenario { .id = "vttest.11.5.iso6429-cursor",
                   .title = "Test ISO-6429 cursor-movement",
                   .keys = Iso6429CursorKeys,
                   .kind = ScenarioKind::Visual,
                   .minimumLevel = VTType::VT100,
                   .driveMode = DriveMode::Live,
                   .gatesBuild = true },
        // Chapter 11.6, one scenario per real test. @see Iso6429ColorPatternKeys for why `*` cannot be
        // used here -- it would run the colour chapter with colour switched off.
        Scenario { .id = "vttest.11.6.2.color-test-pattern",
                   .title = "Display color test-pattern",
                   .keys = Iso6429ColorPatternKeys,
                   .kind = ScenarioKind::Visual,
                   .minimumLevel = VTType::VT100,
                   .driveMode = DriveMode::Live,
                   .gatesBuild = true },
        Scenario { .id = "vttest.11.6.3.sgr-0-color-reset",
                   .title = "Test SGR-0 color reset",
                   .keys = Iso6429ColorSgr0Keys,
                   .kind = ScenarioKind::Visual,
                   .minimumLevel = VTType::VT100,
                   .driveMode = DriveMode::Live,
                   .gatesBuild = true },
        Scenario { .id = "vttest.11.6.4.bce-erase",
                   .title = "Test BCE-style clear line/display (ED, EL)",
                   .keys = Iso6429ColorBceEraseKeys,
                   .kind = ScenarioKind::Visual,
                   .minimumLevel = VTType::VT100,
                   .driveMode = DriveMode::Live,
                   .gatesBuild = true },
        Scenario { .id = "vttest.11.6.5.bce-ech-indexing",
                   .title = "Test BCE-style clear line/display (ECH, Indexing)",
                   .keys = Iso6429ColorBceEchKeys,
                   .kind = ScenarioKind::Visual,
                   .minimumLevel = VTType::VT100,
                   .driveMode = DriveMode::Live,
                   .gatesBuild = true },
        Scenario { .id = "vttest.11.6.6.vt102-with-bce",
                   .title = "Test of VT102-style features with BCE",
                   .keys = Iso6429ColorVt102Keys,
                   .kind = ScenarioKind::Visual,
                   .minimumLevel = VTType::VT100,
                   .driveMode = DriveMode::Live,
                   .gatesBuild = true },
        Scenario { .id = "vttest.11.6.7.ecma48-misc-with-bce",
                   .title = "Test other ISO-6429 features with BCE",
                   .keys = Iso6429ColorEcma48Keys,
                   .kind = ScenarioKind::Visual,
                   .minimumLevel = VTType::VT100,
                   .driveMode = DriveMode::Live,
                   .gatesBuild = true },
        Scenario { .id = "vttest.11.6.7.1.protected-area-with-bce",
                   .title = "Test of ISO-6429 protected areas (SPA), with BCE",
                   .keys = Iso6429ColorProtectedKeys,
                   .kind = ScenarioKind::Visual,
                   .minimumLevel = VTType::VT100,
                   .driveMode = DriveMode::Live,
                   .gatesBuild = true },
        Scenario { .id = "vttest.11.6.8.screen-features-with-bce",
                   .title = "Test screen features with BCE",
                   .keys = Iso6429ColorScreenKeys,
                   .kind = ScenarioKind::Visual,
                   .minimumLevel = VTType::VT100,
                   .driveMode = DriveMode::Live,
                   .gatesBuild = true },
        Scenario { .id = "vttest.11.6.9.sgr-22-27",
                   .title = "Test screen features with ISO 6429 SGR 22-27 codes",
                   .keys = Iso6429ColorSgr2227Keys,
                   .kind = ScenarioKind::Visual,
                   .minimumLevel = VTType::VT100,
                   .driveMode = DriveMode::Live,
                   .gatesBuild = true },
        // The Protected-Area item chapter 11.7's `*` cannot reach. @see Iso6429ProtectedKeys.
        Scenario { .id = "vttest.11.7.1.protected-area",
                   .title = "Test of ISO-6429 protected areas (SPA)",
                   .keys = Iso6429ProtectedKeys,
                   .kind = ScenarioKind::Visual,
                   .minimumLevel = VTType::VT100,
                   .driveMode = DriveMode::Live,
                   .gatesBuild = true },
        Scenario { .id = "vttest.11.7.iso6429-other",
                   .title = "Test other ISO-6429 features",
                   .keys = Iso6429OtherKeys,
                   .kind = ScenarioKind::Visual,
                   .minimumLevel = VTType::VT100,
                   .driveMode = DriveMode::Live,
                   .gatesBuild = true },
        // Contains mouse-tracking sub-tests that wait on real mouse events.
        Scenario { .id = "vttest.11.8.xterm",
                   .title = "Test XTERM special features",
                   .keys = XtermKeys,
                   .kind = ScenarioKind::Interactive,
                   .minimumLevel = VTType::VT100 },
    };
} // namespace

std::span<Prompt const> vtTestPrompts() noexcept
{
    return Prompts;
}

Suite const& vtTestSuite() noexcept
{
    // vttest's geometry argument is "<lines>x<min-columns>.<max-columns>". Contour advertises
    // 132-column capability in DA1, so the suite exercises it.
    static auto const suite = Suite {
        .name = "vttest",
        .program = "vttest",
        .arguments = { "24x80.132" },
        .logFlag = "-l"sv,
        // vttest replays a recorded session with `-c`, and its command-file format is its own
        // transcript format -- which is what lets a Replay scenario be driven without typing at it.
        .cmdFlag = "-c"sv,
        .pageSize = PageSize { .lines = LineCount(24), .columns = ColumnCount(80) },
        .scenarios = Scenarios,
        .prompts = Prompts,
    };
    return suite;
}

} // namespace vtconformance
