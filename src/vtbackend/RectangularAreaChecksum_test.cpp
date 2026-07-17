// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/RectangularAreaChecksum.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace vtbackend;
using namespace std::string_literals;

// Every expected value in this file was measured against xterm-406 (patch level 406, Fedora), driven
// headlessly under Xvfb with `allowWindowOps` on and `checksumExtension` set to the flags under test.
// They are transcripts of a reference terminal, not derivations from its source -- which matters,
// because two of them (the 7-bit mask, and the first-cell exemption) contradict a plain reading of
// the algorithm.

namespace
{

/// A cell as the tests spell it: text, plus optional attributes. Empty text = never written to.
struct TestCell
{
    std::u32string text;
    CellFlags flags {};
};

/// Feeds `rows` through the accumulator and returns what DECRQCRA would report.
[[nodiscard]] uint16_t checksumOf(std::vector<std::vector<TestCell>> const& rows, ChecksumFlags flags = {})
{
    auto checksum = RectangularAreaChecksum { flags };
    for (auto const& row: rows)
    {
        for (auto const& cell: row)
            checksum.addCell(ChecksumCell { .codepoints = cell.text, .flags = cell.flags });
        checksum.endOfLine();
    }
    return checksum.result();
}

/// A single row of cells, each holding one character of `text`.
[[nodiscard]] std::vector<std::vector<TestCell>> row(std::u32string const& text)
{
    auto cells = std::vector<TestCell> {};
    for (auto const codepoint: text)
        cells.push_back(TestCell { .text = std::u32string(1, codepoint) });
    return { cells };
}

/// A single row of `count` cells that were never written to.
[[nodiscard]] std::vector<std::vector<TestCell>> undrawnRow(size_t count)
{
    return { std::vector<TestCell>(count) };
}

constexpr auto Empty = ChecksumFlags {};

} // namespace

TEST_CASE("RectangularAreaChecksum.default_is_the_negated_sum", "[checksum]")
{
    // xterm: 'a' -> FF9F, "ab" -> FF3D.
    CHECK(checksumOf(row(U"a")) == 0xFF9F);
    CHECK(checksumOf(row(U"ab")) == 0xFF3D);

    SECTION("the sum is two's complement, masked to 16 bits")
    {
        CHECK(checksumOf(row(U"a")) == static_cast<uint16_t>(-0x61));
        CHECK(checksumOf(row(U"ab")) == static_cast<uint16_t>(-(0x61 + 0x62)));
    }

    SECTION("multiple rows accumulate")
    {
        // xterm: a 2x2 rectangle holding "ab" / "cd" -> FE76.
        CHECK(checksumOf({ { { .text = U"a" }, { .text = U"b" } }, { { .text = U"c" }, { .text = U"d" } } })
              == 0xFE76);
    }
}

TEST_CASE("RectangularAreaChecksum.a_written_space_is_not_an_empty_cell", "[checksum]")
{
    // The distinction the whole algorithm turns on: a cell holding a space counts (0x20); a cell that
    // was never written to contributes nothing at all.
    CHECK(checksumOf(row(U" ")) == 0xFFE0);     // xterm: written space -> FFE0
    CHECK(checksumOf(undrawnRow(1)) == 0x0000); // xterm: empty -> 0000
    CHECK(checksumOf(undrawnRow(3)) == 0x0000);

    SECTION("undrawn cells drop out of a rectangle that also holds text")
    {
        // xterm: 'a','b',<empty> -> FF3D, i.e. the same as "ab" alone.
        CHECK(checksumOf({ { { .text = U"a" }, { .text = U"b" }, {} } }) == 0xFF3D);
    }
}

TEST_CASE("RectangularAreaChecksum.video_attributes_fold_into_the_cell_value", "[checksum]")
{
    auto const attributed = [](CellFlag flag) {
        return checksumOf({ { TestCell { .text = U"a", .flags = CellFlags { flag } } } });
    };

    // Each weight was measured on xterm by writing 'a' under the matching SGR.
    CHECK(attributed(CellFlag::CharacterProtected) == 0xFF9B); // -(0x61 + 0x04)
    CHECK(attributed(CellFlag::Hidden) == 0xFF97);             // -(0x61 + 0x08)
    CHECK(attributed(CellFlag::Underline) == 0xFF8F);          // -(0x61 + 0x10)
    CHECK(attributed(CellFlag::Inverse) == 0xFF7F);            // -(0x61 + 0x20)
    CHECK(attributed(CellFlag::Blinking) == 0xFF5F);           // -(0x61 + 0x40)
    CHECK(attributed(CellFlag::Bold) == 0xFF1F);               // -(0x61 + 0x80)

    SECTION("SGR 6 weighs the same as SGR 5, because DEC had a single blink")
    {
        CHECK(attributed(CellFlag::RapidBlinking) == 0xFF5F); // xterm: rapid blink -> FF5F, as blink
    }

    SECTION("a cell that is both blinking and rapid-blinking is still charged once")
    {
        auto const both = CellFlags { CellFlag::Blinking } | CellFlag::RapidBlinking;
        CHECK(checksumOf({ { TestCell { .text = U"a", .flags = both } } }) == 0xFF5F);
    }

    SECTION("attributes with no DEC counterpart weigh nothing")
    {
        // xterm: italic and crossed-out 'a' both -> FF9F, i.e. as if plain.
        CHECK(attributed(CellFlag::Italic) == 0xFF9F);
        CHECK(attributed(CellFlag::CrossedOut) == 0xFF9F);
        CHECK(attributed(CellFlag::Framed) == 0xFF9F);
    }

    SECTION("weights add up")
    {
        auto const flags = CellFlags { CellFlag::Bold } | CellFlag::Underline | CellFlag::Inverse;
        CHECK(checksumOf({ { TestCell { .text = U"a", .flags = flags } } })
              == static_cast<uint16_t>(-(0x61 + 0x80 + 0x10 + 0x20)));
    }
}

TEST_CASE("RectangularAreaChecksum.codepoints_are_mapped_into_the_DEC_charset", "[checksum]")
{
    // A DEC terminal reports a 7-bit charset code, so Latin-1 is masked rather than reported as-is:
    // xterm answers 0x69 for U+00E9, not 0xE9. Lossy on purpose.
    CHECK(checksumOf(row(U"\u00E9")) == 0xFF97); // -(0xE9 & 0x7F) == -0x69

    SECTION("anything outside Latin-1 is reported as ESC")
    {
        CHECK(checksumOf(row(U"\u2592")) == 0xFFE5); // U+2592 -> -0x1B
        CHECK(checksumOf(row(U"\U0001F600")) == 0xFFE5);
    }

    SECTION("the mapping is lossy enough to turn a non-breaking space into a blank")
    {
        // U+00A0 masks to 0x20, so it contributes what a written space does rather than its own
        // value. Note it still *counts*: trimming omits blanks that were never drawn, and a blank
        // the application actually wrote is drawn.
        CHECK(checksumOf({ { { .text = U"a" }, { .text = U"\u00A0" } } })
              == static_cast<uint16_t>(-(0x61 + 0x20)));
    }
}

TEST_CASE("RectangularAreaChecksum.Positive_reports_the_plain_sum", "[checksum]")
{
    constexpr auto Flags = ChecksumFlags { ChecksumFlag::Positive };

    CHECK(checksumOf(row(U"a"), Flags) == 0x0061);
    CHECK(checksumOf(row(U"ab"), Flags) == 0x00C3);
    CHECK(checksumOf(row(U" "), Flags) == 0x0020);
    CHECK(checksumOf(row(U"\u00E9"), Flags) == 0x0069);
    CHECK(checksumOf(row(U"\u2592"), Flags) == 0x001B);
    CHECK(checksumOf({ { TestCell { .text = U"a", .flags = CellFlags { CellFlag::Bold } } } }, Flags)
          == 0x00E1);
}

TEST_CASE("RectangularAreaChecksum.NoAttributes_leaves_the_video_attributes_out", "[checksum]")
{
    constexpr auto Flags = ChecksumFlags { ChecksumFlag::NoAttributes };

    // xterm with checksumExtension=2: bold 'a' checksums as plain 'a'.
    CHECK(checksumOf({ { TestCell { .text = U"a", .flags = CellFlags { CellFlag::Bold } } } }, Flags)
          == 0xFF9F);
    CHECK(checksumOf({ { TestCell { .text = U"a", .flags = CellFlags { CellFlag::CharacterProtected } } } },
                     Flags)
          == 0xFF9F);
}

TEST_CASE("RectangularAreaChecksum.IncludeUndrawn_counts_never_written_cells", "[checksum]")
{
    constexpr auto Flags = ChecksumFlags { ChecksumFlag::IncludeUndrawn };

    CHECK(checksumOf(undrawnRow(1), Flags) == 0xFFE0); // xterm: -0x20

    SECTION("but only the first one -- three undrawn cells checksum the same as one")
    {
        // This is xterm's behaviour, verified: `checksumExtension=8` answers FFE0 for a rectangle of
        // two, three, or four undrawn cells alike. It falls out of the first-cell exemption in the
        // blank-trimming rule, and looks arbitrary because it is. Reproduced so that our checksums
        // stay byte-identical to the reference.
        CHECK(checksumOf(undrawnRow(2), Flags) == 0xFFE0);
        CHECK(checksumOf(undrawnRow(3), Flags) == 0xFFE0);

        // ... and the exemption is per rectangle, not per row: a 2x2 of undrawn cells is still -0x20.
        CHECK(checksumOf({ { {}, {} }, { {}, {} } }, Flags) == 0xFFE0);
    }

    SECTION("a drawn cell after an undrawn one still counts")
    {
        // xterm, checksumExtension=8, rectangle [empty; 'a'] -> FF7F == -(0x20 + 0x61).
        CHECK(checksumOf({ { TestCell {} }, { TestCell { .text = U"a" } } }, Flags) == 0xFF7F);
    }
}

TEST_CASE("RectangularAreaChecksum.KeepBlanks_counts_every_cell", "[checksum]")
{
    constexpr auto Flags = ChecksumFlags { ChecksumFlag::KeepBlanks };

    // xterm, checksumExtension=4: blanks stop being omitted, and undrawn cells read as blanks.
    CHECK(checksumOf(undrawnRow(2), Flags) == 0xFFC0);              // -(2 * 0x20)
    CHECK(checksumOf(undrawnRow(3), Flags) == 0xFFA0);              // -(3 * 0x20)
    CHECK(checksumOf({ { {}, {} }, { {}, {} } }, Flags) == 0xFF80); // 2x2 -> -(4 * 0x20)

    SECTION("blanks between text are counted too")
    {
        // xterm: 'a','b',<empty> -> FF1D == -(0x61 + 0x62 + 0x20).
        CHECK(checksumOf({ { { .text = U"a" }, { .text = U"b" }, {} } }, Flags) == 0xFF1D);
    }
}

TEST_CASE("RectangularAreaChecksum.RawCodepoint_reports_the_codepoint_verbatim", "[checksum]")
{
    constexpr auto Flags = ChecksumFlags { ChecksumFlag::RawCodepoint };

    // xterm, checksumExtension=16: no charset mapping, no 7-bit mask.
    CHECK(checksumOf(row(U"\u00E9"), Flags) == 0xFF17); // -0xE9, not -0x69
    CHECK(checksumOf(row(U"\u2592"), Flags) == 0xDA6E); // -0x2592, not -0x1B

    SECTION("it does not change any of the other axes")
    {
        CHECK(checksumOf(undrawnRow(2), Flags) == 0x0000); // still skipped
        CHECK(checksumOf({ { TestCell { .text = U"a", .flags = CellFlags { CellFlag::Bold } } } }, Flags)
              == 0xFF1F); // attributes still folded in
    }
}

TEST_CASE("RectangularAreaChecksum.flags_compose", "[checksum]")
{
    // The combination esctest needs, and the one Contour's conformance harness configures: undrawn
    // cells read as blanks, and video attributes are left out so a protected cell still reads as its
    // character. Verified end to end: xterm with checksumExtension=10 passes esctest's ED/DECSED
    // suites 27/27, and with anything else fails them.
    constexpr auto Flags = ChecksumFlags { ChecksumFlag::NoAttributes } | ChecksumFlag::IncludeUndrawn;

    CHECK(checksumOf(undrawnRow(1), Flags) == 0xFFE0);
    CHECK(checksumOf(row(U"a"), Flags) == 0xFF9F);
    CHECK(checksumOf({ { TestCell { .text = U"a", .flags = CellFlags { CellFlag::CharacterProtected } } } },
                     Flags)
          == 0xFF9F);

    SECTION("Positive and NoAttributes together give the plain character value")
    {
        constexpr auto PlainFlags = ChecksumFlags { ChecksumFlag::Positive } | ChecksumFlag::NoAttributes;
        CHECK(checksumOf({ { TestCell { .text = U"a", .flags = CellFlags { CellFlag::Bold } } } }, PlainFlags)
              == 0x0061);
    }
}

TEST_CASE("RectangularAreaChecksum.empty_rectangle_checksums_to_zero", "[checksum]")
{
    CHECK(checksumOf({}, Empty) == 0x0000);
    CHECK(checksumOf({}, ChecksumFlags { ChecksumFlag::KeepBlanks }) == 0x0000);
    CHECK(checksumOf({}, ChecksumFlags { ChecksumFlag::Positive }) == 0x0000);
}

TEST_CASE("RectangularAreaChecksum.combining_marks", "[checksum]")
{
    // xterm adds combining marks to its untrimmed running total only, so with the default (trimming)
    // flags they never reach the reported checksum -- its source carries a `FIXME - not counted if
    // trimming blanks` on exactly this line. Matched deliberately: being byte-compatible with the
    // reference beats being quietly better than it.
    // 'e' followed by U+0301 COMBINING ACUTE ACCENT: one cell holding two codepoints.
    auto const combining = std::vector<std::vector<TestCell>> { { { .text = U"e\u0301" } } };

    CHECK(checksumOf(combining) == 0xFF9B); // -'e' only == -0x65

    SECTION("KeepBlanks reports the total, which does include them")
    {
        CHECK(checksumOf(combining, ChecksumFlags { ChecksumFlag::KeepBlanks })
              == static_cast<uint16_t>(-(0x65 + 0x0301)));
    }

    SECTION("RawCodepoint drops them entirely")
    {
        constexpr auto Flags = ChecksumFlags { ChecksumFlag::RawCodepoint } | ChecksumFlag::KeepBlanks;
        CHECK(checksumOf(combining, Flags) == static_cast<uint16_t>(-0x65));
    }
}
