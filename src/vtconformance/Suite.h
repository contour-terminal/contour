// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/InputGenerator.h>
#include <vtbackend/RectangularAreaChecksum.h>
#include <vtbackend/VTType.h>

#include <vtpty/PageSize.h>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vtconformance
{

/// What kind of oracle a scenario can be judged by.
enum class ScenarioKind : std::uint8_t
{
    /// The test program checks itself and prints its own verdict; no golden file needed.
    SelfChecking,

    /// The test program draws something a human would judge. Judged here by a blessed screen dump.
    Visual,

    /// The test program needs real key presses or mouse events, which a headless engine cannot
    /// supply. Reported as skipped, never as passed — a skipped test must not look like a green one.
    Interactive,
};

/// How much driving the program under test needs.
enum class DriveMode : std::uint8_t
{
    /// It walks menus and comes to rest at prompts, which the driver answers on seeing the marker
    /// vttest printed (vttest).
    ///
    /// The barrier is the byte stream, which is the only channel honest about it: vttest flushes
    /// stdout before every blocking read, but never flushes its log, and `readnl()` -- the read behind
    /// every hold -- announces nothing at all. So the marker on the wire IS the fact that it is about
    /// to block, and typing on it is provably safe, because `holdit()` runs its `inflush()` BEFORE
    /// printing the banner (unix_io.c:239-241).
    ///
    /// This replaced a driver that inferred "the screen is final" from output falling quiet for 150ms.
    /// It is the same policy -- the same prompts, answered in the same order -- so every scenario kept
    /// its goldens through the move, which is how the replacement was proven. @see MarkerScanner.
    Live,

    /// It reads its own keystrokes from a recorded command file and runs to completion (vttest -c).
    ///
    /// Deterministic: the driver types nothing, so there is no barrier to get wrong and no screen to
    /// sample early. vttest brackets every read of a *terminal reply* with pause_replay()/
    /// resume_replay(), so DA/DSR/DECRQSS answers still come from the live engine -- navigation is
    /// scripted while the measurement stays real. The price is vttest's hard-coded 1s sleep per
    /// replayed answer.
    Replay,

    /// It drives itself to completion and reports its own results; the only barrier is its exit
    /// (esctest). Nothing is typed at it, so nothing can be mistyped -- which is why a self-asserting
    /// suite is worth far more per test than a human-judged one.
    Unattended,
};

/// One test to run: a program driven to a point, then judged.
///
/// This is the data half of the harness. Adding a vttest chapter means adding a row to a table; no
/// harness logic changes.
struct Scenario
{
    /// Stable identifier, also the golden file's and command file's basename, e.g.
    /// "vttest.01.cursor-movements".
    std::string_view id;

    /// Human-readable title, taken from the test program's own menu.
    std::string_view title;

    /// The keystrokes that walk the program's menus to this test and back out again. Each entry is
    /// written to the program's stdin once it has come to rest at an input prompt.
    ///
    /// Only consulted when driving `Live`. A `Replay` scenario's keys live in its recorded
    /// command file instead -- they must, because a command file needs one entry per read vttest
    /// performs, including every "Push <RETURN>", and that count is a property of the chapter rather
    /// than of its menu path.
    std::span<std::string_view const> keys;

    ScenarioKind kind = ScenarioKind::Visual;

    /// The conformance level the terminal must report for this test to be reachable at all.
    vtbackend::VTType minimumLevel = vtbackend::VTType::VT100;

    DriveMode driveMode = DriveMode::Live;

    /// Whether a failing run of THIS scenario should fail the build.
    ///
    /// Gating is per scenario, not per suite, because within one suite the oracles differ in how far
    /// they can be trusted. A `SelfChecking` scenario driven by `Replay` is judged by the program's
    /// own verdicts, read from its transcript after it exits -- no goldens, no screen sampling, no
    /// timing -- so it gates.
    ///
    /// A `Visual` one is judged against a screen dump, and used not to. The reason was the driver:
    /// dumps were sampled on a 150ms silence timeout, so a mismatch could mean the harness blinked
    /// rather than the engine changed, and failing a build on that would punish the engine for the
    /// harness's own doubt. **That driver is gone.** Every scenario is captured at a causal barrier now
    /// -- vttest's own flushed prompt on the wire -- and the whole suite report is byte-identical
    /// across runs, so a mismatch means the engine moved.
    ///
    /// Note what this does and does not claim. Many of these screens are *recorded* rather than
    /// *reviewed*: they pin today's behaviour without asserting it is correct, and the review is
    /// tracked per chapter alongside the goldens. @see docs/internals/vt-conformance.md. Gating them is still
    /// right -- "is this screen correct?" and "should a change to it be visible?" are different
    /// questions, and a recorded golden answers the second perfectly well. If a frozen screen turns
    /// out to be wrong, the gate is what makes fixing it a deliberate, reviewed re-bless instead of a
    /// silent drift.
    bool gatesBuild = false;
};

/// What the driver should do when the program under test stops at a given prompt.
enum class PromptAction : std::uint8_t
{
    /// A menu. Consume the scenario's next key and type it.
    Menu,

    /// A "press RETURN to continue" barrier. The screen is final and is exactly what a human would
    /// have been asked to judge, so this is where a visual scenario captures its dump.
    Hold,

    /// Type the prompt's own `input` verbatim.
    ///
    /// Used to walk out of sub-tests that ask for real key presses. Escaping such a test is not the
    /// same as exercising it -- @see docs/internals/vt-conformance.md -- but it is far better than wedging
    /// the driver, because it lets every *other* item in the same chapter run.
    Type,

    /// Press the prompt's `key`, encoded by the terminal's own input generator.
    ///
    /// For a prompt whose whole point is *what a key sends*. vttest's LNM test sets New Line mode,
    /// asks for RETURN and demands CR LF, then resets it and demands CR alone (reports.c:604,617) --
    /// it is asking the terminal a question about its keyboard encoding. Answering with `Type` and a
    /// literal would make the test assert the driver's own string; answering with the key lets the
    /// engine's LNM state decide, which is the thing being measured.
    Key,
};

/// A prompt the program under test comes to rest at, and how to answer it.
struct Prompt
{
    /// Matched as a substring of the child's byte stream, not of the rendered screen.
    ///
    /// The wire is the only channel honest about the prompt: it carries the marker at the moment the
    /// child flushed it, which is the moment it is about to block. @see MarkerScanner, DriveMode::Live.
    std::string_view pattern;

    PromptAction action;

    /// What to type. Only meaningful for `PromptAction::Type`.
    std::string_view input {};

    /// Which key to press. Only meaningful for `PromptAction::Key`.
    vtbackend::Key key {};
};

/// A whole conformance suite: one external program plus the scenarios that drive it.
struct Suite
{
    /// Suite name, as used on the command line (`--suite=vttest`).
    std::string_view name;

    /// The program to spawn. Resolved against PATH, or overridden on the command line.
    std::string_view program;

    /// Arguments passed to every run of the program. The harness appends its own logging flags.
    std::vector<std::string> arguments;

    /// The flag with which the program is told where to write its transcript.
    std::string_view logFlag = "-l";

    /// The flag with which the program is told to replay a recorded command file.
    ///
    /// vttest's command-file format *is* its transcript format (it looks for the next `Read:` line
    /// and skips any `Wait:`/`Done:` span), so a recorded transcript replays verbatim. Empty for a
    /// program with no such mechanism.
    std::string_view cmdFlag {};

    /// The flag with which the program can be told to run only some of its own tests, if it has one.
    ///
    /// Not the same thing as the scenario filter: this narrows the work *inside* one run, which is
    /// what makes a single failing esctest module reproducible on its own.
    std::string_view testFilterFlag {};

    /// A file that must exist in the suite directory for the suite to be runnable.
    ///
    /// Empty when the program is simply expected on PATH. esctest is GPL-2.0 and Contour is
    /// Apache-2.0, so it is fetched rather than vendored, and its absence must degrade to a skip
    /// rather than to a failure.
    std::string_view entryPoint {};

    /// The page size the terminal is created with. vttest assumes 24x80 with 132-column capability.
    vtpty::PageSize pageSize;

    /// The scenarios, in the order they should run.
    std::span<Scenario const> scenarios;

    /// The prompts the program comes to rest at. Empty for an unattended suite.
    std::span<Prompt const> prompts {};

    /// The XTCHECKSUM extension the terminal is configured with for this suite.
    ///
    /// A suite that reads the screen back through DECRQCRA has to agree with the terminal about what
    /// a checksum means. esctest does, and needs `NoAttributes | IncludeUndrawn` -- the same
    /// configuration a real xterm needs in order to pass it.
    vtbackend::ChecksumFlags checksumExtension {};
};

/// The prompts vttest blocks on, and how to answer each.
///
/// vttest always comes to rest blocked on stdin at one of these, so the marker arriving on the wire means
/// "the screen is final, and it is your turn". That is a causal barrier, not a guess: `holdit()` flushes
/// stdout before the blocking read, so the marker cannot arrive early. Nothing here is timing-based —
/// the driver that inferred finality from output falling quiet is gone. @see Scenario::gatesBuild.
///
/// Only consulted for `DriveMode::Live`: under `Replay` vttest answers its own prompts from the
/// command file, so the driver types nothing at all.
///
/// Adding a prompt is adding a row; the driver has no per-prompt logic.
[[nodiscard]] std::span<Prompt const> vtTestPrompts() noexcept;

/// The vttest suite: its scenario table and geometry.
[[nodiscard]] Suite const& vtTestSuite() noexcept;

/// The esctest suite.
///
/// esctest (github.com/ThomasDickey/esctest2) is worth far more per test than vttest is, because it
/// asserts rather than draws: it reads the screen back through DECRQCRA and compares it to what it
/// expects, so it needs no golden files, no screen dumps and no human. The price is that it is
/// GPL-2.0 while Contour is Apache-2.0, so it is fetched (`scripts/fetch-esctest.sh`) and invoked as
/// an external program -- never vendored.
[[nodiscard]] Suite const& escTestSuite() noexcept;

/// @return The suite of that name, or nullptr.
[[nodiscard]] Suite const* findSuite(std::string_view name) noexcept;

} // namespace vtconformance
