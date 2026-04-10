// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/LineSoA.h>
#include <vtbackend/SoAClusterWriter.h>

#include <catch2/catch_test_macros.hpp>

#include <string_view>

using namespace vtbackend;
using namespace std::string_view_literals;

// =============================================================================
// writeTextToSoA — ASCII fast path
// =============================================================================

TEST_CASE("writeTextToSoA.ascii", "[SoAWriter]")
{
    LineSoA line;
    initializeLineSoA(line, ColumnCount(20));

    auto const attrs = GraphicsAttributes { .foregroundColor = Color::Indexed(3) };
    auto const written = writeTextToSoA(line, 0, "Hello", attrs);

    CHECK(written == 5);
    CHECK(line.codepoints[0] == U'H');
    CHECK(line.codepoints[1] == U'e');
    CHECK(line.codepoints[2] == U'l');
    CHECK(line.codepoints[3] == U'l');
    CHECK(line.codepoints[4] == U'o');

    for (size_t i = 0; i < 5; ++i)
    {
        CHECK(line.sgr[i].foregroundColor == Color::Indexed(3));
        CHECK(line.widths[i] == 1);
        CHECK(line.clusterSize[i] == 1);
    }

    CHECK(line.codepoints[5] == 0);
}

TEST_CASE("writeTextToSoA.ascii.atOffset", "[SoAWriter]")
{
    LineSoA line;
    initializeLineSoA(line, ColumnCount(20));

    auto const written = writeTextToSoA(line, 5, "Hi", GraphicsAttributes {});

    CHECK(written == 2);
    CHECK(line.codepoints[5] == U'H');
    CHECK(line.codepoints[6] == U'i');
    CHECK(line.codepoints[4] == 0); // before offset untouched
}

TEST_CASE("writeTextToSoA.ascii.withHyperlink", "[SoAWriter]")
{
    LineSoA line;
    initializeLineSoA(line, ColumnCount(10));

    auto const hlId = HyperlinkId(42);
    writeTextToSoA(line, 0, "AB", GraphicsAttributes {}, hlId);

    CHECK(line.hyperlinks[0] == hlId);
    CHECK(line.hyperlinks[1] == hlId);
}

// =============================================================================
// writeTextToSoA — non-ASCII (UTF-8 decode + grapheme segmentation)
// =============================================================================

TEST_CASE("writeTextToSoA.utf8.singleCodepoint", "[SoAWriter]")
{
    LineSoA line;
    initializeLineSoA(line, ColumnCount(10));

    // ü = U+00FC, UTF-8: 0xC3 0xBC, width 1
    auto const written = writeTextToSoA(line, 0, "\xC3\xBC"sv, GraphicsAttributes {});

    CHECK(written == 1);
    CHECK(line.codepoints[0] == U'\u00FC');
    CHECK(line.widths[0] == 1);
    CHECK(line.clusterSize[0] == 1);
}

TEST_CASE("writeTextToSoA.utf8.precomposed", "[SoAWriter]")
{
    LineSoA line;
    initializeLineSoA(line, ColumnCount(10));

    // "café" = c a f é(U+00E9), UTF-8: 63 61 66 C3 A9
    auto const written = writeTextToSoA(line, 0, "caf\xC3\xA9"sv, GraphicsAttributes {});

    CHECK(written == 4);
    CHECK(line.codepoints[0] == U'c');
    CHECK(line.codepoints[1] == U'a');
    CHECK(line.codepoints[2] == U'f');
    CHECK(line.codepoints[3] == U'\u00E9');
    CHECK(line.clusterSize[3] == 1);
}

TEST_CASE("writeTextToSoA.utf8.decomposed", "[SoAWriter]")
{
    LineSoA line;
    initializeLineSoA(line, ColumnCount(10));

    // é decomposed: e(U+0065) + combining acute(U+0301)
    // UTF-8: 65 CC 81
    auto const written = writeTextToSoA(line, 0, "\x65\xCC\x81"sv, GraphicsAttributes {});

    CHECK(written == 1);
    CHECK(line.codepoints[0] == U'e');
    CHECK(line.clusterSize[0] == 2);
    CHECK(line.clusterPool.size() == 1);
    CHECK(line.clusterPool[0] == U'\u0301');
}

TEST_CASE("writeTextToSoA.utf8.cjkWide", "[SoAWriter]")
{
    LineSoA line;
    initializeLineSoA(line, ColumnCount(10));

    // 漢 = U+6F22, East Asian Wide (width 2), UTF-8: E6 BC A2
    auto const written = writeTextToSoA(line, 0, "\xE6\xBC\xA2"sv, GraphicsAttributes {});

    CHECK(written == 2);
    CHECK(line.codepoints[0] == U'\u6F22');
    CHECK(line.widths[0] == 2);
    CHECK(line.clusterSize[0] == 1);

    // Second cell = wide char continuation
    CHECK(line.codepoints[1] == 0);
    CHECK(line.sgr[1].flags.contains(CellFlag::WideCharContinuation));
}

TEST_CASE("writeTextToSoA.utf8.twoCjk", "[SoAWriter]")
{
    LineSoA line;
    initializeLineSoA(line, ColumnCount(10));

    // 漢字: two CJK characters, each width 2 = 4 columns total
    // 漢 U+6F22 = E6 BC A2, 字 U+5B57 = E5 AD 97
    auto const written = writeTextToSoA(line, 0, "\xE6\xBC\xA2\xE5\xAD\x97"sv, GraphicsAttributes {});

    CHECK(written == 4);

    CHECK(line.codepoints[0] == U'\u6F22');
    CHECK(line.widths[0] == 2);
    CHECK(line.codepoints[1] == 0);
    CHECK(line.sgr[1].flags.contains(CellFlag::WideCharContinuation));

    CHECK(line.codepoints[2] == U'\u5B57');
    CHECK(line.widths[2] == 2);
    CHECK(line.codepoints[3] == 0);
    CHECK(line.sgr[3].flags.contains(CellFlag::WideCharContinuation));
}

TEST_CASE("writeTextToSoA.utf8.mixedAsciiCjk", "[SoAWriter]")
{
    LineSoA line;
    initializeLineSoA(line, ColumnCount(20));

    // "Hi 漢!" — ASCII + CJK wide + ASCII
    auto const written = writeTextToSoA(line, 0, "Hi \xE6\xBC\xA2!"sv, GraphicsAttributes {});

    CHECK(written == 6);
    CHECK(line.codepoints[0] == U'H');
    CHECK(line.codepoints[1] == U'i');
    CHECK(line.codepoints[2] == U' ');
    CHECK(line.codepoints[3] == U'\u6F22');
    CHECK(line.widths[3] == 2);
    CHECK(line.sgr[4].flags.contains(CellFlag::WideCharContinuation));
    CHECK(line.codepoints[5] == U'!');
}

// =============================================================================
// writeCellToSoA — direct cell write
// =============================================================================

TEST_CASE("writeCellToSoA.basic", "[SoAWriter]")
{
    LineSoA line;
    initializeLineSoA(line, ColumnCount(10));

    auto const attrs =
        GraphicsAttributes { .foregroundColor = Color::Indexed(5), .backgroundColor = Color::Indexed(6) };
    writeCellToSoA(line, 3, U'X', 1, attrs);

    CHECK(line.codepoints[3] == U'X');
    CHECK(line.widths[3] == 1);
    CHECK(line.sgr[3].foregroundColor == Color::Indexed(5));
    CHECK(line.sgr[3].backgroundColor == Color::Indexed(6));
    CHECK(line.clusterSize[3] == 1);
}

TEST_CASE("writeCellToSoA.empty", "[SoAWriter]")
{
    LineSoA line;
    initializeLineSoA(line, ColumnCount(10));

    writeCellToSoA(line, 0, char32_t { 0 }, 1, GraphicsAttributes {});
    CHECK(line.clusterSize[0] == 0);
}

// =============================================================================
// fillWideCharContinuation
// =============================================================================

TEST_CASE("fillWideCharContinuation.basic", "[SoAWriter]")
{
    LineSoA line;
    initializeLineSoA(line, ColumnCount(10));

    auto const attrs = GraphicsAttributes { .foregroundColor = Color::Indexed(2) };
    fillWideCharContinuation(line, 1, 1, attrs);

    CHECK(line.codepoints[1] == 0);
    CHECK(line.sgr[1].flags.contains(CellFlag::WideCharContinuation));
    CHECK(line.sgr[1].foregroundColor == Color::Indexed(2));
}
