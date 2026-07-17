// SPDX-License-Identifier: Apache-2.0
#include <array>
#include <string_view>

#include <vtconformance/Suite.h>

using namespace std::string_view_literals;
using vtbackend::ChecksumFlag;
using vtbackend::ChecksumFlags;
using vtbackend::VTType;
using vtpty::ColumnCount;
using vtpty::LineCount;
using vtpty::PageSize;

namespace vtconformance
{

namespace
{
    // esctest needs no driving and no golden files: it walks its own test list, reads the screen back
    // through DECRQCRA, and prints a verdict per test. So there is one scenario, and its report is
    // esctest's own -- see EscTestLog.
    //
    // Splitting it per test module would buy nothing: the whole run is a single process that never
    // blocks on us, and esctest already names every test it fails.
    constexpr auto Scenarios = std::array {
        Scenario { .id = "esctest.all",
                   .title = "esctest: every test module",
                   .keys = {},
                   .kind = ScenarioKind::SelfChecking,
                   .minimumLevel = VTType::VT100,
                   .driveMode = DriveMode::Unattended,
                   .gatesBuild = true },
    };
} // namespace

Suite const& escTestSuite() noexcept
{
    static auto const suite = Suite {
        .name = "esctest",

        // esctest is a Python program that runs *inside* the terminal under test: it writes escape
        // sequences to its stdout and reads the terminal's replies from its stdin. Spawning the
        // interpreter on the PTY is therefore all the integration it needs.
        .program = "python3",
        // --xterm-reverse-wrap tells esctest which xterm its reverse-wrap expectations should follow.
        // Left at its default of 0, it expects DEC mode 45 to wrap backwards over *any* line, wrapped or
        // not -- which is what xterm did up to patch 380. Patch 383 split that in two: mode 45 follows
        // only a line the text actually wrapped onto, and the unconditional form moved to mode 1045.
        // Contour implements the split, as every terminal written since does, so esctest is told which
        // xterm to hold it to. This is the same kind of knob as checksumExtension below: a real xterm
        // fails these tests at esctest's default too.
        .arguments = { "esctest.py",
                       // Contour is measured as itself, not as xterm. It aims to be xterm-compatible for
                       // every value a test reads back, but it is a distinct terminal for knownBug()
                       // purposes -- where it is frequently *more* correct than xterm (xterm's own bugs
                       // must not oblige Contour to be buggy too). The contour-aware esctest2 fork adds
                       // this terminal profile. @see scripts/fetch-esctest.sh.
                       "--expected-terminal=contour",
                       "--xterm-reverse-wrap=406",
                       "--no-print-logs" },
        .logFlag = "--logfile"sv,
        .testFilterFlag = "--include"sv,
        .entryPoint = "esctest.py"sv,

        // esctest resizes the terminal to 25x80 during setup (`CSI 8 ; 25 ; 80 t`); starting there
        // saves it the round trip.
        .pageSize = PageSize { .lines = LineCount(25), .columns = ColumnCount(80) },
        .scenarios = Scenarios,
        .prompts = {},

        // Without this esctest cannot pass *any* terminal, xterm included: it reads a cell at a time
        // through DECRQCRA and compares the answer to the character it expects, so it needs cells
        // that were never written to read as blanks (IncludeUndrawn) and a cell's video attributes
        // kept out of its value (NoAttributes) -- otherwise a protected cell reads as its character
        // plus four. Verified against xterm-406: `checksumExtension: 10` passes its ED/DECSED suites
        // 27/27, the default passes 0/28.
        .checksumExtension = ChecksumFlags { ChecksumFlag::NoAttributes } | ChecksumFlag::IncludeUndrawn,
    };
    return suite;
}

Suite const* findSuite(std::string_view name) noexcept
{
    for (auto const* suite: { &vtTestSuite(), &escTestSuite() })
        if (suite->name == name)
            return suite;
    return nullptr;
}

} // namespace vtconformance
