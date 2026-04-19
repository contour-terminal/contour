// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/CellProxy.h>
#include <vtbackend/Line.h>
#include <vtbackend/LineSoA.h>

#include <catch2/catch_test_macros.hpp>

#include <format>

using namespace vtbackend;

// =============================================================================
// LineSoA tests
// =============================================================================

TEST_CASE("LineSoA.initializeLineSoA", "[LineSoA]")
{
    LineSoA line;
    initializeLineSoA(line, ColumnCount(80));

    CHECK(line.codepoints.size() == 80);
    CHECK(line.widths.size() == 80);
    CHECK(line.sgr.size() == 80);
    CHECK(line.hyperlinks.size() == 80);
    CHECK(line.clusterSize.size() == 80);
    CHECK(line.clusterPoolIndex.size() == 80);

    // All codepoints should be zero (empty)
    for (size_t i = 0; i < 80; ++i)
    {
        CHECK(line.codepoints[i] == 0);
        CHECK(line.widths[i] == 1);
        CHECK(line.clusterSize[i] == 0);
    }

    CHECK(line.clusterPool.empty());
    CHECK(!line.imageFragments);
    CHECK(line.usedColumns == ColumnCount(0));
}

TEST_CASE("LineSoA.initializeLineSoA.withAttrs", "[LineSoA]")
{
    auto const attrs = GraphicsAttributes { .foregroundColor = Color::Indexed(1),
                                            .backgroundColor = Color::Indexed(2),
                                            .underlineColor = Color::Indexed(3),
                                            .flags = CellFlag::Bold };

    LineSoA line;
    initializeLineSoA(line, ColumnCount(10), attrs);

    for (size_t i = 0; i < 10; ++i)
    {
        CHECK(line.sgr[i].foregroundColor == Color::Indexed(1));
        CHECK(line.sgr[i].backgroundColor == Color::Indexed(2));
        CHECK(line.sgr[i].underlineColor == Color::Indexed(3));
        CHECK(line.sgr[i].flags.contains(CellFlag::Bold));
    }
}

TEST_CASE("LineSoA.clearRange", "[LineSoA]")
{
    LineSoA line;
    initializeLineSoA(line, ColumnCount(10));

    // Write some data first
    line.codepoints[3] = U'A';
    line.codepoints[4] = U'B';
    line.codepoints[5] = U'C';
    line.clusterSize[3] = 1;
    line.clusterSize[4] = 1;
    line.clusterSize[5] = 1;
    line.sgr[3].foregroundColor = Color::Indexed(5);
    line.sgr[4].foregroundColor = Color::Indexed(5);
    line.sgr[5].foregroundColor = Color::Indexed(5);

    // Clear columns 3-5
    auto const clearAttrs = GraphicsAttributes { .foregroundColor = Color::Indexed(7) };
    clearRange(line, 3, 3, clearAttrs);

    CHECK(line.codepoints[3] == 0);
    CHECK(line.codepoints[4] == 0);
    CHECK(line.codepoints[5] == 0);
    CHECK(line.clusterSize[3] == 0);
    CHECK(line.sgr[3].foregroundColor == Color::Indexed(7));
    CHECK(line.sgr[4].foregroundColor == Color::Indexed(7));
}

TEST_CASE("LineSoA.trimBlankRight", "[LineSoA]")
{
    LineSoA line;
    initializeLineSoA(line, ColumnCount(10));

    CHECK(trimBlankRight(line, 10) == 0);

    line.codepoints[0] = U'A';
    line.clusterSize[0] = 1;
    CHECK(trimBlankRight(line, 10) == 1);

    line.codepoints[7] = U'Z';
    line.clusterSize[7] = 1;
    CHECK(trimBlankRight(line, 10) == 8);
}

TEST_CASE("LineSoA.copyColumns", "[LineSoA]")
{
    LineSoA src;
    initializeLineSoA(src, ColumnCount(10));

    src.codepoints[0] = U'H';
    src.codepoints[1] = U'i';
    src.clusterSize[0] = 1;
    src.clusterSize[1] = 1;
    src.widths[0] = 1;
    src.widths[1] = 1;
    src.sgr[0].foregroundColor = Color::Indexed(3);
    src.sgr[1].foregroundColor = Color::Indexed(4);

    LineSoA dst;
    initializeLineSoA(dst, ColumnCount(10));

    copyColumns(src, 0, dst, 5, 2);

    CHECK(dst.codepoints[5] == U'H');
    CHECK(dst.codepoints[6] == U'i');
    CHECK(dst.clusterSize[5] == 1);
    CHECK(dst.clusterSize[6] == 1);
    CHECK(dst.sgr[5].foregroundColor == Color::Indexed(3));
    CHECK(dst.sgr[6].foregroundColor == Color::Indexed(4));
    // Original positions should still be empty
    CHECK(dst.codepoints[0] == 0);
}

TEST_CASE("LineSoA.copyColumns.trivialInvalidated", "[LineSoA]")
{
    // Source line with non-uniform SGR
    LineSoA src;
    initializeLineSoA(src, ColumnCount(10));
    src.codepoints[0] = U'A';
    src.clusterSize[0] = 1;
    src.sgr[0].foregroundColor = Color::Indexed(1);

    // Destination line starts trivial
    LineSoA dst;
    initializeLineSoA(dst, ColumnCount(10));
    REQUIRE(dst.trivial);

    copyColumns(src, 0, dst, 0, 1);

    // After copying non-uniform data, trivial must be false
    CHECK_FALSE(dst.trivial);
}

TEST_CASE("LineSoA.copyColumns.trivialInvalidated.partialCopy", "[LineSoA]")
{
    // Simulates horizontal-margin scroll: copy columns 2-7 from source into destination
    auto const srcAttrs = GraphicsAttributes { .foregroundColor = Color::Indexed(5) };
    LineSoA src;
    initializeLineSoA(src, ColumnCount(10), srcAttrs);
    src.codepoints[2] = U'X';
    src.clusterSize[2] = 1;

    LineSoA dst;
    initializeLineSoA(dst, ColumnCount(10));
    REQUIRE(dst.trivial);

    // Partial copy: columns 2-7 from source into destination at same position
    copyColumns(src, 2, dst, 2, 6);

    // Destination now has mixed SGR (cols 0-1 default, cols 2-7 indexed(5)), trivial must be false
    CHECK_FALSE(dst.trivial);
}

TEST_CASE("LineSoA.resizeLineSoA.grow", "[LineSoA]")
{
    LineSoA line;
    initializeLineSoA(line, ColumnCount(5));
    line.codepoints[2] = U'X';
    line.clusterSize[2] = 1;

    resizeLineSoA(line, ColumnCount(10));

    CHECK(line.codepoints.size() == 10);
    CHECK(line.codepoints[2] == U'X'); // preserved
    CHECK(line.codepoints[7] == 0);    // new columns are empty
}

TEST_CASE("LineSoA.resizeLineSoA.grow.trivialInvalidatedOnMismatch", "[LineSoA]")
{
    auto const attrs = GraphicsAttributes { .foregroundColor = Color::Indexed(3) };
    LineSoA line;
    initializeLineSoA(line, ColumnCount(5), attrs);
    REQUIRE(line.trivial);

    // Growing with default fillAttrs ({}) differs from the existing sgr[0].
    resizeLineSoA(line, ColumnCount(10));

    CHECK_FALSE(line.trivial);
}

TEST_CASE("LineSoA.resizeLineSoA.grow.trivialPreservedOnMatch", "[LineSoA]")
{
    auto const attrs = GraphicsAttributes { .foregroundColor = Color::Indexed(3) };
    LineSoA line;
    initializeLineSoA(line, ColumnCount(5), attrs);
    REQUIRE(line.trivial);

    // Growing with same fillAttrs keeps trivial.
    resizeLineSoA(line, ColumnCount(10), attrs);

    CHECK(line.trivial);
}

TEST_CASE("LineSoA.resizeLineSoA.shrink", "[LineSoA]")
{
    LineSoA line;
    initializeLineSoA(line, ColumnCount(10));
    line.codepoints[3] = U'A';
    line.clusterSize[3] = 1;
    line.usedColumns = ColumnCount(8);

    resizeLineSoA(line, ColumnCount(5));

    CHECK(line.codepoints.size() == 5);
    CHECK(line.codepoints[3] == U'A');         // preserved (within new range)
    CHECK(line.usedColumns == ColumnCount(5)); // clamped
}

// =============================================================================
// Grapheme cluster pool tests
// =============================================================================

TEST_CASE("LineSoA.graphemeCluster.singleCodepoint", "[LineSoA]")
{
    LineSoA line;
    initializeLineSoA(line, ColumnCount(10));

    line.codepoints[0] = U'A';
    line.clusterSize[0] = 1;

    size_t count = 0;
    forEachCodepoint(line, 0, [&](char32_t cp) {
        CHECK(cp == U'A');
        ++count;
    });
    CHECK(count == 1);
}

TEST_CASE("LineSoA.graphemeCluster.multiCodepoint", "[LineSoA]")
{
    LineSoA line;
    initializeLineSoA(line, ColumnCount(10));

    // Simulate e + combining acute accent (é)
    line.codepoints[0] = U'e';
    line.clusterSize[0] = 1;

    auto const widthChange = appendCodepointToCluster(line, 0, U'\u0301'); // combining acute

    CHECK(widthChange == 0);
    CHECK(line.clusterSize[0] == 2);
    CHECK(line.clusterPool.size() == 1);
    CHECK(line.clusterPool[0] == U'\u0301');

    std::vector<char32_t> collected;
    forEachCodepoint(line, 0, [&](char32_t cp) { collected.push_back(cp); });
    REQUIRE(collected.size() == 2);
    CHECK(collected[0] == U'e');
    CHECK(collected[1] == U'\u0301');
}

TEST_CASE("LineSoA.graphemeCluster.clearExtras", "[LineSoA]")
{
    LineSoA line;
    initializeLineSoA(line, ColumnCount(10));

    line.codepoints[0] = U'e';
    line.clusterSize[0] = 1;
    appendCodepointToCluster(line, 0, U'\u0301');
    CHECK(line.clusterSize[0] == 2);

    clearClusterExtras(line, 0);
    CHECK(line.clusterSize[0] == 1); // back to single codepoint
    // Pool still has the old entry (lazy compaction), but it's unreachable
    CHECK(line.clusterPool.size() == 1);
}

TEST_CASE("LineSoA.graphemeCluster.poolClearedOnReset", "[LineSoA]")
{
    LineSoA line;
    initializeLineSoA(line, ColumnCount(10));

    line.codepoints[0] = U'e';
    line.clusterSize[0] = 1;
    appendCodepointToCluster(line, 0, U'\u0301');

    resetLine(line, ColumnCount(10));

    CHECK(line.clusterPool.empty());
    CHECK(line.clusterSize[0] == 0);
}

TEST_CASE("LineSoA.graphemeCluster.maxCodepoints", "[LineSoA]")
{
    LineSoA line;
    initializeLineSoA(line, ColumnCount(10));

    line.codepoints[0] = U'A';
    line.clusterSize[0] = 1;

    // Append up to MaxGraphemeClusterSize - 1 extra codepoints
    for (uint8_t i = 1; i < MaxGraphemeClusterSize; ++i)
        appendCodepointToCluster(line, 0, static_cast<char32_t>(0x0300 + i));

    CHECK(line.clusterSize[0] == MaxGraphemeClusterSize);

    // Attempting to exceed the limit should be a no-op
    auto const result = appendCodepointToCluster(line, 0, U'\u0310');
    CHECK(result == 0);
    CHECK(line.clusterSize[0] == MaxGraphemeClusterSize);
}

// =============================================================================
// CellProxy tests
// =============================================================================

TEST_CASE("CellProxy.readWrite", "[CellProxy]")
{
    LineSoA line;
    initializeLineSoA(line, ColumnCount(10));

    auto cell = CellProxy(line, 3);
    CHECK(cell.empty());
    CHECK(cell.codepointCount() == 0);
    CHECK(cell.width() == 1);

    auto const attrs =
        GraphicsAttributes { .foregroundColor = Color::Indexed(1), .backgroundColor = Color::Indexed(2) };
    cell.write(attrs, U'X', 1);

    CHECK(!cell.empty());
    CHECK(cell.codepoint() == U'X');
    CHECK(cell.codepointCount() == 1);
    CHECK(cell.foregroundColor() == Color::Indexed(1));
    CHECK(cell.backgroundColor() == Color::Indexed(2));
}

TEST_CASE("CellProxy.writeWithHyperlink", "[CellProxy]")
{
    LineSoA line;
    initializeLineSoA(line, ColumnCount(10));

    auto cell = CellProxy(line, 0);
    auto const attrs = GraphicsAttributes {};
    auto const hlId = HyperlinkId(42);
    cell.write(attrs, U'L', 1, hlId);

    CHECK(cell.hyperlink() == hlId);
}

TEST_CASE("CellProxy.reset", "[CellProxy]")
{
    LineSoA line;
    initializeLineSoA(line, ColumnCount(10));

    auto cell = CellProxy(line, 0);
    auto const attrs = GraphicsAttributes { .foregroundColor = Color::Indexed(5) };
    cell.write(attrs, U'A', 1);
    CHECK(!cell.empty());

    cell.reset();
    CHECK(cell.empty());
    CHECK(cell.codepoint() == 0);
    CHECK(cell.foregroundColor() == DefaultColor());
}

TEST_CASE("CellProxy.resetWithAttrs", "[CellProxy]")
{
    LineSoA line;
    initializeLineSoA(line, ColumnCount(10));

    auto cell = CellProxy(line, 0);
    auto const attrs = GraphicsAttributes { .foregroundColor = Color::Indexed(9) };
    cell.reset(attrs);

    CHECK(cell.empty());
    CHECK(cell.foregroundColor() == Color::Indexed(9));
}

TEST_CASE("CellProxy.appendCharacter", "[CellProxy]")
{
    LineSoA line;
    initializeLineSoA(line, ColumnCount(10));

    auto cell = CellProxy(line, 0);
    cell.write(GraphicsAttributes {}, U'e', 1);
    (void) cell.appendCharacter(U'\u0301');

    CHECK(cell.codepointCount() == 2);
    CHECK(cell.codepoint(0) == U'e');
    CHECK(cell.codepoint(1) == U'\u0301');

    auto const text = cell.toUtf8();
    // Decomposed form: 'e' (0x65) + combining acute accent (U+0301 = 0xCC 0x81)
    CHECK(text == "e\xCC\x81");
}

TEST_CASE("CellProxy.setCharacter", "[CellProxy]")
{
    LineSoA line;
    initializeLineSoA(line, ColumnCount(10));

    auto cell = CellProxy(line, 0);
    cell.setCharacter(U'W');
    CHECK(cell.codepoint() == U'W');
    CHECK(cell.width() == 1);
    CHECK(cell.codepointCount() == 1);
}

TEST_CASE("CellProxy.flags", "[CellProxy]")
{
    LineSoA line;
    initializeLineSoA(line, ColumnCount(10));

    auto cell = CellProxy(line, 0);
    CHECK(!cell.isFlagEnabled(CellFlag::Bold));

    auto const attrs = GraphicsAttributes { .flags = CellFlags { CellFlag::Bold, CellFlag::Italic } };
    cell.write(attrs, U'B', 1);

    CHECK(cell.isFlagEnabled(CellFlag::Bold));
    CHECK(cell.isFlagEnabled(CellFlag::Italic));
    CHECK(!cell.isFlagEnabled(CellFlag::Underline));
}

TEST_CASE("CellProxy.codepoints", "[CellProxy]")
{
    LineSoA line;
    initializeLineSoA(line, ColumnCount(10));

    auto cell = CellProxy(line, 0);
    cell.write(GraphicsAttributes {}, U'e', 1);
    (void) cell.appendCharacter(U'\u0301');

    auto const cps = cell.codepoints();
    REQUIRE(cps.size() == 2);
    CHECK(cps[0] == U'e');
    CHECK(cps[1] == U'\u0301');
}

TEST_CASE("CellProxy.beginsWith", "[CellProxy]")
{
    LineSoA line;
    initializeLineSoA(line, ColumnCount(10));

    auto cell = CellProxy(line, 0);
    cell.write(GraphicsAttributes {}, U'A', 1);

    CHECK(beginsWith(U"ABC", cell));
    CHECK(beginsWith(U"A", cell));
    CHECK(!beginsWith(U"B", cell));
}

// =============================================================================
// CellProxy trivial flag invalidation tests
// =============================================================================

TEST_CASE("CellProxy.trivialInvalidation.atCol0", "[CellProxy]")
{
    LineSoA line;
    initializeLineSoA(line, ColumnCount(10));
    CellProxy(line, 0).write(GraphicsAttributes {}, U'A', 1);
    CellProxy(line, 1).write(GraphicsAttributes {}, U'B', 1);
    REQUIRE(line.trivial); // uniform SGR — still trivial

    // Changing SGR at col 0 must invalidate trivial (col 1 still has default attrs)
    CellProxy(line, 0).setForegroundColor(Color::Indexed(IndexedColor::Red));
    CHECK_FALSE(line.trivial);
}

TEST_CASE("CellProxy.trivialInvalidation.setForegroundColor", "[CellProxy]")
{
    LineSoA line;
    initializeLineSoA(line, ColumnCount(10));
    CellProxy(line, 0).write(GraphicsAttributes {}, U'A', 1);
    CellProxy(line, 1).write(GraphicsAttributes {}, U'B', 1);
    REQUIRE(line.trivial); // uniform SGR — still trivial

    CellProxy(line, 1).setForegroundColor(Color::Indexed(IndexedColor::Red));
    CHECK_FALSE(line.trivial);
}

TEST_CASE("CellProxy.trivialInvalidation.setBackgroundColor", "[CellProxy]")
{
    LineSoA line;
    initializeLineSoA(line, ColumnCount(10));
    CellProxy(line, 0).write(GraphicsAttributes {}, U'A', 1);
    CellProxy(line, 1).write(GraphicsAttributes {}, U'B', 1);
    REQUIRE(line.trivial);

    CellProxy(line, 1).setBackgroundColor(Color::Indexed(IndexedColor::Blue));
    CHECK_FALSE(line.trivial);
}

TEST_CASE("CellProxy.trivialInvalidation.setUnderlineColor", "[CellProxy]")
{
    LineSoA line;
    initializeLineSoA(line, ColumnCount(10));
    CellProxy(line, 0).write(GraphicsAttributes {}, U'A', 1);
    CellProxy(line, 1).write(GraphicsAttributes {}, U'B', 1);
    REQUIRE(line.trivial);

    CellProxy(line, 1).setUnderlineColor(Color::Indexed(IndexedColor::Green));
    CHECK_FALSE(line.trivial);
}

TEST_CASE("CellProxy.trivialInvalidation.resetFlags", "[CellProxy]")
{
    LineSoA line;
    initializeLineSoA(line, ColumnCount(10));
    CellProxy(line, 0).write(GraphicsAttributes {}, U'A', 1);
    CellProxy(line, 1).write(GraphicsAttributes {}, U'B', 1);
    REQUIRE(line.trivial);

    CellProxy(line, 1).resetFlags(CellFlags { CellFlag::Bold });
    CHECK_FALSE(line.trivial);
}

TEST_CASE("CellProxy.trivialInvalidation.setGraphicsRendition", "[CellProxy]")
{
    LineSoA line;
    initializeLineSoA(line, ColumnCount(10));
    CellProxy(line, 0).write(GraphicsAttributes {}, U'A', 1);
    CellProxy(line, 1).write(GraphicsAttributes {}, U'B', 1);
    REQUIRE(line.trivial);

    CellProxy(line, 1).setGraphicsRendition(GraphicsRendition::Bold);
    CHECK_FALSE(line.trivial);
}

TEST_CASE("CellProxy.trivialInvalidation.setHyperlink", "[CellProxy]")
{
    LineSoA line;
    initializeLineSoA(line, ColumnCount(10));
    CellProxy(line, 0).write(GraphicsAttributes {}, U'A', 1);
    CellProxy(line, 1).write(GraphicsAttributes {}, U'B', 1);
    REQUIRE(line.trivial);

    CellProxy(line, 1).setHyperlink(HyperlinkId(42));
    CHECK_FALSE(line.trivial);
}

// =============================================================================
// LineFlags formatter tests
// =============================================================================

TEST_CASE("LineFlags.formatter.OutputStart", "[LineFlags]")
{
    CHECK(std::format("{}", LineFlags(LineFlag::OutputStart)) == "OutputStart");
}

TEST_CASE("LineFlags.formatter.CommandEnd", "[LineFlags]")
{
    CHECK(std::format("{}", LineFlags(LineFlag::CommandEnd)) == "CommandEnd");
}

TEST_CASE("LineFlags.formatter.composite", "[LineFlags]")
{
    auto const flags = LineFlags({ LineFlag::Wrapped, LineFlag::OutputStart });
    auto const result = std::format("{}", flags);
    CHECK(result == "Wrapped,OutputStart");
}

// ---------------------------------------------------------------------------
// trivialBuffer() regression test: fill attributes must survive empty lines
// ---------------------------------------------------------------------------

TEST_CASE("Line.trivialBuffer.emptyLinePreservesFillAttributes", "[Line]")
{
    // Regression test: when a trivial line is reset/cleared with non-default
    // SGR attributes but has no text content (all codepoints zero),
    // trivialBuffer() must still return those SGR attributes as fill attributes.
    // Previously it returned default GraphicsAttributes{} when used==0.

    auto const navyBg =
        GraphicsAttributes { .foregroundColor = Color::Indexed(7), .backgroundColor = Color::Indexed(4) };

    auto constexpr Cols = ColumnCount(80);
    auto line = Line(Cols, LineFlag::None, navyBg);

    // Verify the line is trivial and has no text content.
    REQUIRE(line.isTrivialBuffer());

    std::u32string textOut;
    auto const tb = line.trivialBuffer(textOut);

    // The line has zero used columns (no codepoints set).
    CHECK(tb.usedColumns == ColumnCount(0));
    CHECK(textOut.empty());

    // Critical: fill attributes must match the SGR the line was initialized with,
    // NOT default GraphicsAttributes{}.
    CHECK(tb.fillAttributes.backgroundColor == Color::Indexed(4));
    CHECK(tb.fillAttributes.foregroundColor == Color::Indexed(7));
    CHECK(tb.textAttributes.backgroundColor == Color::Indexed(4));
    CHECK(tb.textAttributes.foregroundColor == Color::Indexed(7));
}

TEST_CASE("Line.trivialBuffer.resetPreservesFillAttributes", "[Line]")
{
    // Verify that resetting a line with specific SGR and then calling
    // trivialBuffer() returns those attributes even when codepoints are all zero.

    auto constexpr Cols = ColumnCount(40);
    auto line = Line(Cols, LineFlag::None, GraphicsAttributes {});

    // Write some content first (materialize the lazy-blank line first).
    line.materializedStorage().codepoints[0] = U'X';
    line.materializedStorage().clusterSize[0] = 1;

    // Now reset the line with a themed background (simulating EL CSI K at col 0).
    auto const themedAttrs = GraphicsAttributes { .backgroundColor = Color::Indexed(4) };
    line.reset(LineFlag::None, themedAttrs);

    REQUIRE(line.isTrivialBuffer());

    std::u32string textOut;
    auto const tb = line.trivialBuffer(textOut);

    CHECK(tb.usedColumns == ColumnCount(0));
    CHECK(tb.fillAttributes.backgroundColor == Color::Indexed(4));
    CHECK(tb.textAttributes.backgroundColor == Color::Indexed(4));
}

// LineView tests removed — Line is now backed by LineSoA directly (tested via Line_test.cpp)
