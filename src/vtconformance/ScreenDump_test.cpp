// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/CellFlags.h>
#include <vtbackend/MockTerm.h>

#include <catch2/catch_test_macros.hpp>

#include <format>

#include <vtconformance/ScreenDump.h>

using namespace std::string_view_literals;
using namespace vtconformance;

namespace
{
[[nodiscard]] auto makeTerm()
{
    return vtbackend::MockTerm { vtbackend::PageSize { vtbackend::LineCount(3), vtbackend::ColumnCount(8) } };
}
} // namespace

TEST_CASE("ScreenDump.text plane", "[vtconformance]")
{
    auto mock = makeTerm();
    mock.writeToScreen("AB"sv);

    auto const dump =
        dumpScreen(mock.terminal,
                   DumpOptions { .attributes = false, .lineFlags = false, .modes = false, .cursor = false });

    CHECK(dump.find("@geometry 3x8") != std::string::npos);

    // A never-written cell and an explicitly written space look the same on screen but are distinct
    // to DECRQCRA and to selective erase, so the dump must keep them apart.
    CHECK(dump.find("01|AB······") != std::string::npos);
}

TEST_CASE("ScreenDump.a written space is not an untouched cell", "[vtconformance]")
{
    auto mock = makeTerm();
    mock.writeToScreen("A B"sv);

    auto const dump =
        dumpScreen(mock.terminal,
                   DumpOptions { .attributes = false, .lineFlags = false, .modes = false, .cursor = false });

    CHECK(dump.find("01|A␣B·····") != std::string::npos);
}

TEST_CASE("ScreenDump.attribute plane and legend", "[vtconformance]")
{
    auto mock = makeTerm();
    mock.writeToScreen("\033[1mA\033[0mB"sv); // bold A, plain B

    auto const dump =
        dumpScreen(mock.terminal,
                   DumpOptions { .attributes = true, .lineFlags = false, .modes = false, .cursor = false });

    CHECK(dump.find("---- attributes ") != std::string::npos);
    CHECK(dump.find("01|A.......") != std::string::npos);
    CHECK(dump.find("A = Bold") != std::string::npos);
    CHECK(dump.find(". = default") != std::string::npos);
}

TEST_CASE("ScreenDump.line flags record double-width and double-height", "[vtconformance]")
{
    auto mock = makeTerm();
    mock.writeToScreen("\033#6X"sv); // DECDWL on line 1

    auto const dump =
        dumpScreen(mock.terminal,
                   DumpOptions { .attributes = false, .lineFlags = true, .modes = false, .cursor = false });

    CHECK(dump.find("---- lineflags ") != std::string::npos);
    CHECK(dump.find("01|DoubleWidth") != std::string::npos);
}

TEST_CASE("ScreenDump.mode plane", "[vtconformance]")
{
    auto mock = makeTerm();
    mock.writeToScreen("\033[?6h"sv); // DECOM on

    auto const dump =
        dumpScreen(mock.terminal,
                   DumpOptions { .attributes = false, .lineFlags = false, .modes = true, .cursor = false });

    CHECK(dump.find("DECOM=on") != std::string::npos);
    CHECK(dump.find("DECAWM=") != std::string::npos);
}

TEST_CASE("ScreenDump.cursor position is 1-based, like the VT sequences that move it", "[vtconformance]")
{
    auto mock = makeTerm();
    mock.writeToScreen("\033[2;3H"sv); // CUP row 2, column 3

    auto const dump =
        dumpScreen(mock.terminal,
                   DumpOptions { .attributes = false, .lineFlags = false, .modes = false, .cursor = true });

    CHECK(dump.find("@cursor line=2 column=3") != std::string::npos);
}

TEST_CASE("ScreenDump.dumps are deterministic", "[vtconformance]")
{
    auto mock = makeTerm();
    mock.writeToScreen("\033[31mred\033[0m plain"sv);

    CHECK(dumpScreen(mock.terminal) == dumpScreen(mock.terminal));
}

TEST_CASE("ScreenDump.diffDumps", "[vtconformance]")
{
    SECTION("identical dumps have no diff")
    {
        CHECK(diffDumps("a\nb\n"sv, "a\nb\n"sv).empty());
    }

    SECTION("a differing line is reported with both sides")
    {
        auto const diff = diffDumps("a\nb\n"sv, "a\nc\n"sv);
        CHECK(diff.find("@@ line 2") != std::string::npos);
        CHECK(diff.find("-b") != std::string::npos);
        CHECK(diff.find("+c") != std::string::npos);
    }
}

TEST_CASE("ScreenDump.every cell flag is named in the legend", "[vtconformance]")
{
    // The guard for a defect that reached a committed golden: `vttest.11.7.1.protected-area.step04`
    // recorded the line `A = default`, which is a contradiction on its face -- the legend reserves `.`
    // for the default rendition, so a symbol was handed out precisely *because* the cell was not
    // default, and then described as if it were. The cause was a hand-copied list of flags to iterate
    // that never learnt about `CharacterProtectedISO`, so the protected-area goldens could not tell a
    // protected cell from an unprotected one -- the exact thing that chapter tests.
    //
    // Driven from the backend's own table, so a flag added tomorrow and left unnamed fails here rather
    // than silently flattening a future golden. @see VTBACKEND_CELL_FLAGS.
    for (auto const flag: vtbackend::CellFlagList)
    {
        // Structural, and deliberately not a rendition. @see ScreenDump.cpp's describe().
        if (flag == vtbackend::CellFlag::WideCharContinuation)
            continue;

        INFO(std::format("flag: {}", flag));
        CHECK(std::format("{}", flag) != "default");
    }
}

TEST_CASE("ScreenDump.an ISO-protected cell is distinguishable from a plain one", "[vtconformance]")
{
    // vttest's chapter 11.7.1 writes protected and unprotected text side by side and erases across
    // both; a dump that renders them identically cannot witness the difference. SPA/EPA (ISO 6429)
    // protection is a *separate* flag from DECSCA's, because the two are honoured by opposite erase
    // families -- so it needs its own legend entry, not DECSCA's.
    auto mock = makeTerm();
    mock.writeToScreen("\033V"sv); // SPA -- start of protected area
    mock.writeToScreen("P"sv);
    mock.writeToScreen("\033W"sv); // EPA -- end of protected area
    mock.writeToScreen("U"sv);

    auto const dump = dumpScreen(mock.terminal, DumpOptions { .lineFlags = false, .modes = false });

    INFO(dump);
    CHECK(dump.find("= CharacterProtectedISO") != std::string::npos);
    // The two cells must not collapse onto one legend symbol.
    CHECK(dump.find("A = default") == std::string::npos);
}
