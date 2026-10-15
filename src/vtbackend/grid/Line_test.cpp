// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/grid/Line.hpp>

#include <core/Escape.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <format>
#include <ranges>
#include <tuple>
#include <utility>
#include <vector>

using namespace std;

using namespace vtbackend;
using namespace crispy;

TEST_CASE("Line.wrappedFlag", "[Line]")
{
    auto line = Line(ColumnCount(10), LineFlag::Wrapped, GraphicsAttributes {});
    CHECK(line.wrapped());
    CHECK(line.wrappedFlag() == LineFlag::Wrapped);
}

TEST_CASE("Line.resize", "[Line]")
{
    auto constexpr DisplayWidth = ColumnCount(4);

    auto const sgr = GraphicsAttributes {};
    auto lineSoA = Line(DisplayWidth, LineFlag::None, sgr);
    CHECK(lineSoA.size() == DisplayWidth);

    lineSoA.resize(ColumnCount(10));
    CHECK(lineSoA.size() == ColumnCount(10));

    lineSoA.resize(ColumnCount(5));
    CHECK(lineSoA.size() == ColumnCount(5));

    lineSoA.resize(ColumnCount(3));
    CHECK(lineSoA.size() == ColumnCount(3));
}

TEST_CASE("Line.reflow", "[Line]")
{
    auto constexpr DisplayWidth = ColumnCount(4);

    auto const sgr = GraphicsAttributes {};
    auto lineSoA = Line(DisplayWidth, LineFlag::Wrappable, sgr);

    // Write "abcd" into the line via SoA — materialize first since the line is lazy-blank
    auto& storage = lineSoA.materializedStorage();
    storage.codepoints[0] = 'a';
    storage.codepoints[1] = 'b';
    storage.codepoints[2] = 'c';
    storage.codepoints[3] = 'd';
    for (size_t i = 0; i < 4; ++i)
        storage.clusterSize[i] = 1;

    CHECK(lineSoA.toUtf8() == "abcd");

    auto overflow = lineSoA.reflow(ColumnCount(5));
    CHECK(overflow.codepoints.empty()); // no overflow
    CHECK(lineSoA.size() == ColumnCount(5));

    // Reset for reflow-shrink test
    lineSoA = Line(DisplayWidth, LineFlag::Wrappable, sgr);
    auto& s2 = lineSoA.materializedStorage();
    s2.codepoints[0] = 'a';
    s2.codepoints[1] = 'b';
    s2.codepoints[2] = 'c';
    s2.codepoints[3] = 'd';
    for (size_t i = 0; i < 4; ++i)
        s2.clusterSize[i] = 1;

    overflow = lineSoA.reflow(ColumnCount(3));
    CHECK(lineSoA.size() == ColumnCount(3));
    CHECK(overflow.codepoints.size() == 1); // 'd' overflowed
}

TEST_CASE("Line.SoA.basic", "[Line]")
{
    auto constexpr DisplayWidth = ColumnCount(10);

    auto sgr = GraphicsAttributes {};
    sgr.foregroundColor = RGBColor(0x123456);
    sgr.backgroundColor = Color::Indexed(IndexedColor::Yellow);
    sgr.underlineColor = Color::Indexed(IndexedColor::Red);
    sgr.flags |= CellFlag::CurlyUnderlined;

    auto line = Line(DisplayWidth, LineFlag::None, sgr);

    // Write some text via CellProxy
    for (size_t i = 0; i < 10; ++i)
    {
        auto cell = line.useCellAt(ColumnOffset::cast_from(i));
        cell.write(sgr, static_cast<char32_t>('0' + i), 1);
    }

    // Verify via CellProxy
    for (size_t i = 0; i < 10; ++i)
    {
        auto cell = line.useCellAt(ColumnOffset::cast_from(i));
        INFO(std::format("column {} codepoint {}", i, (char) cell.codepoint(0)));
        CHECK(cell.foregroundColor() == sgr.foregroundColor);
        CHECK(cell.backgroundColor() == sgr.backgroundColor);
        CHECK(cell.underlineColor() == sgr.underlineColor);
        CHECK(cell.codepointCount() == 1);
        CHECK(char(cell.codepoint(0)) == char('0' + i));
    }
}

TEST_CASE("Line.toUtf8", "[Line]")
{
    auto constexpr DisplayWidth = ColumnCount(5);

    auto line = Line(DisplayWidth, LineFlag::None, GraphicsAttributes {});
    CHECK(line.toUtf8() == "     "); // 5 empty cells -> 5 spaces

    auto cell0 = line.useCellAt(ColumnOffset(0));
    cell0.write(GraphicsAttributes {}, U'H', 1);
    auto cell1 = line.useCellAt(ColumnOffset(1));
    cell1.write(GraphicsAttributes {}, U'i', 1);

    auto const text = line.toUtf8();
    CHECK(text == "Hi   ");

    auto const trimmed = line.toUtf8Trimmed();
    CHECK(trimmed == "Hi");
}

TEST_CASE("Line.toUtf8ColumnAligned", "[Line]")
{
    auto constexpr DisplayWidth = ColumnCount(5);

    auto line = Line(DisplayWidth, LineFlag::None, GraphicsAttributes {});

    // A double-width character (CJK U+4E2D) in columns 0-1, then an ASCII 'X' in column 2.
    line.useCellAt(ColumnOffset(0)).write(GraphicsAttributes {}, U'\x4e2d', 2);
    line.useCellAt(ColumnOffset(2)).write(GraphicsAttributes {}, U'X', 1);

    // toUtf8 collapses the wide character to its single codepoint (what copy/yank and selection
    // want): the continuation cell contributes nothing.
    CHECK(line.toUtf8()
          == "\xe4\xb8\xad"
             "X  ");

    // toUtf8ColumnAligned emits exactly one codepoint per grid column: the continuation cell becomes
    // a space, so 'X' sits at codepoint index 2 (its column) rather than index 1.
    CHECK(line.toUtf8ColumnAligned()
          == "\xe4\xb8\xad"
             " X  ");
}

// ---------------------------------------------------------------------------
// Lazy-blank Line behavior
// ---------------------------------------------------------------------------

TEST_CASE("Line.blank.constructionIsLazy", "[Line][blank]")
{
    auto const sgr = GraphicsAttributes { .backgroundColor = Color::Indexed(4) };
    auto line = Line(ColumnCount(80), LineFlag::Wrappable, sgr);

    REQUIRE(line.isBlank());
    CHECK(line.size() == ColumnCount(80));
    // All six SoA arrays remain at size 0 — no per-column allocation happened.
    CHECK(line.storage().codepoints.empty());
    CHECK(line.storage().widths.empty());
    CHECK(line.storage().sgr.empty());
    CHECK(line.storage().hyperlinks.empty());
    CHECK(line.storage().clusterSize.empty());
    CHECK(line.storage().clusterPoolIndex.empty());
    // fillAttrs is preserved through the lazy state.
    CHECK(line.storage().fillAttrs.backgroundColor == Color::Indexed(4));
    // Read accessors short-circuit safely on the blank state.
    CHECK(line.empty());
    CHECK(line.isTrivialBuffer());
    CHECK(line.cellEmptyAt(ColumnOffset(0)));
    CHECK(line.cellEmptyAt(ColumnOffset(79)));
    CHECK(line.cellWidthAt(ColumnOffset(42)) == 1);
}

TEST_CASE("Line.blank.trivialBufferReturnsFillAttrs", "[Line][blank]")
{
    auto const sgr =
        GraphicsAttributes { .foregroundColor = Color::Indexed(7), .backgroundColor = Color::Indexed(4) };
    auto line = Line(ColumnCount(40), LineFlag::None, sgr);

    REQUIRE(line.isBlank());
    std::u32string text;
    auto const tb = line.trivialBuffer(text);

    CHECK(tb.displayWidth == ColumnCount(40));
    CHECK(tb.usedColumns == ColumnCount(0));
    CHECK(text.empty());
    CHECK(tb.fillAttributes.backgroundColor == Color::Indexed(4));
    CHECK(tb.textAttributes.backgroundColor == Color::Indexed(4));
}

TEST_CASE("Line.blank.toUtf8ReturnsSpaces", "[Line][blank]")
{
    auto line = Line(ColumnCount(5), LineFlag::None, GraphicsAttributes {});

    REQUIRE(line.isBlank());
    CHECK(line.toUtf8() == "     ");
    CHECK(line.toUtf8Trimmed().empty());
}

TEST_CASE("Line.blank.materializeOnUseCellAt", "[Line][blank]")
{
    auto line = Line(ColumnCount(10), LineFlag::None, GraphicsAttributes {});
    REQUIRE(line.isBlank());

    auto cell = line.useCellAt(ColumnOffset(3));
    cell.write(GraphicsAttributes {}, U'X', 1);

    CHECK_FALSE(line.isBlank());
    CHECK(line.toUtf8() == "   X      ");
    CHECK(line.cellEmptyAt(ColumnOffset(0)));
    CHECK_FALSE(line.cellEmptyAt(ColumnOffset(3)));
}

TEST_CASE("Line.blank.materializeOnFillAscii", "[Line][blank]")
{
    auto line = Line(ColumnCount(10), LineFlag::None, GraphicsAttributes {});
    REQUIRE(line.isBlank());

    line.fill(ColumnOffset(2), GraphicsAttributes {}, "abc");

    CHECK_FALSE(line.isBlank());
    CHECK(line.toUtf8() == "  abc     ");
}

TEST_CASE("Line.blank.resetReturnsToBlankWithNewFillAttrs", "[Line][blank]")
{
    auto line = Line(ColumnCount(10), LineFlag::None, GraphicsAttributes {});
    line.useCellAt(ColumnOffset(0)).write(GraphicsAttributes {}, U'A', 1);
    REQUIRE_FALSE(line.isBlank());

    auto const themed = GraphicsAttributes { .backgroundColor = Color::Indexed(2) };
    line.reset(LineFlag::None, themed);

    CHECK(line.isBlank());
    CHECK(line.size() == ColumnCount(10));
    CHECK(line.storage().fillAttrs.backgroundColor == Color::Indexed(2));
}

TEST_CASE("Line.blank.resizeKeepsBlank", "[Line][blank]")
{
    auto line = Line(ColumnCount(80), LineFlag::None, GraphicsAttributes {});
    REQUIRE(line.isBlank());

    line.resize(ColumnCount(200));
    CHECK(line.isBlank());
    CHECK(line.size() == ColumnCount(200));
    CHECK(line.storage().codepoints.empty());

    line.resize(ColumnCount(40));
    CHECK(line.isBlank());
    CHECK(line.size() == ColumnCount(40));
}

TEST_CASE("Line.blank.reflowReturnsEmptyOverflow", "[Line][blank]")
{
    auto line = Line(ColumnCount(80), LineFlag::Wrappable, GraphicsAttributes {});
    REQUIRE(line.isBlank());

    auto overflow = line.reflow(ColumnCount(40));
    CHECK(overflow.codepoints.empty());
    CHECK(line.isBlank());
    CHECK(line.size() == ColumnCount(40));
}

TEST_CASE("Line.blank.copyColumnsFromBlankSourceClearsDest", "[Line][blank]")
{
    auto blankSrc = Line(ColumnCount(10), LineFlag::None, GraphicsAttributes {});
    auto dst = Line(ColumnCount(10), LineFlag::None, GraphicsAttributes {});
    // Materialize destination and write some content first.
    dst.useCellAt(ColumnOffset(0)).write(GraphicsAttributes {}, U'X', 1);
    dst.useCellAt(ColumnOffset(1)).write(GraphicsAttributes {}, U'Y', 1);
    REQUIRE_FALSE(dst.isBlank());

    // Copying from a blank source clears the destination range.
    copyColumns(blankSrc.storage(), 0, dst.materializedStorage(), 0, 5);

    CHECK(dst.cellEmptyAt(ColumnOffset(0)));
    CHECK(dst.cellEmptyAt(ColumnOffset(1)));
    CHECK(dst.cellEmptyAt(ColumnOffset(4)));
}

TEST_CASE("Line.revision.freshLinesArePendingAndStampOnce", "[Line][revision]")
{
    auto line = Line(ColumnCount(8), LineFlag::None, GraphicsAttributes {});
    CHECK(line.isDirty()); // bootstrap self-heals: a fresh line is pending
    CHECK(line.revision() == 0);

    CHECK(line.stampRevision(7));
    CHECK(line.revision() == 7);
    CHECK_FALSE(line.isDirty());

    // A clean line never restamps.
    CHECK_FALSE(line.stampRevision(8));
    CHECK(line.revision() == 7);
}

TEST_CASE("Line.revision.everyMutatorDirties", "[Line][revision]")
{
    auto line = Line(ColumnCount(8), LineFlag::None, GraphicsAttributes {});

    auto const dirtiedBy = [&](auto&& mutate) {
        static_cast<void>(line.stampRevision(1)); // normalize to clean
        mutate();
        return line.isDirty();
    };

    CHECK(dirtiedBy([&] { line.reset(LineFlag::None, GraphicsAttributes {}); }));
    CHECK(dirtiedBy([&] { line.reset(LineFlag::None, GraphicsAttributes {}, ColumnCount(8)); }));
    CHECK(dirtiedBy([&] { line.fill(LineFlag::None, GraphicsAttributes {}, U'x', 1); }));
    CHECK(dirtiedBy([&] { line.fill(ColumnOffset(0), GraphicsAttributes {}, "ab"); }));
    CHECK(dirtiedBy([&] { line.resize(ColumnCount(10)); }));
    // useCellAt defers dirtying to the proxy's write methods: a read-only
    // CellProxy (the return value discarded here) does NOT dirty the line.
    CHECK_FALSE(dirtiedBy([&] { static_cast<void>(line.useCellAt(ColumnOffset(0))); }));
    // Writing through the proxy dirties the line.
    CHECK(dirtiedBy([&] { line.useCellAt(ColumnOffset(0)).write(GraphicsAttributes {}, U'x', 1); }));
    CHECK(dirtiedBy([&] { static_cast<void>(line.materializedStorage()); }));
    CHECK(dirtiedBy([&] { static_cast<void>(line.storage()); })); // mutable overload
    CHECK(dirtiedBy([&] { static_cast<void>(line.flags()); }));   // mutable overload
    CHECK(dirtiedBy([&] { line.setFlag(LineFlag::Marked, true); }));
    CHECK(dirtiedBy([&] { line.setCommandEndOffset(ColumnOffset(3)); }));
    CHECK(dirtiedBy([&] { line.setPromptEndOffset(ColumnOffset(2)); }));
}

TEST_CASE("Line.revision.constReadsStayClean", "[Line][revision]")
{
    auto line = Line(ColumnCount(8), LineFlag::None, GraphicsAttributes {});
    line.fill(ColumnOffset(0), GraphicsAttributes {}, "hi");
    static_cast<void>(line.stampRevision(1));

    auto const& constLine = line;
    static_cast<void>(constLine.flags());
    static_cast<void>(constLine.storage());
    static_cast<void>(constLine.toUtf8());
    static_cast<void>(constLine.isTrivialBuffer());
    CHECK_FALSE(line.isDirty());
}

TEST_CASE("Line.revision.assignmentDirtiesTheDestination", "[Line][revision]")
{
    auto source = Line(ColumnCount(4), LineFlag::None, GraphicsAttributes {});
    source.fill(ColumnOffset(0), GraphicsAttributes {}, "abcd");
    static_cast<void>(source.stampRevision(5));

    auto target = Line(ColumnCount(4), LineFlag::None, GraphicsAttributes {});
    static_cast<void>(target.stampRevision(1));
    REQUIRE_FALSE(target.isDirty());

    SECTION("copy assignment")
    {
        target = source;
    }
    SECTION("move assignment")
    {
        target = std::move(source);
    }
    CHECK(target.isDirty());
    CHECK(target.toUtf8() == "abcd");
}

TEST_CASE("Line.revision.rotateDirtiesEveryMovedRow", "[Line][revision]")
{
    // scrollDown at capacity and margin scrolls move Lines between rows via
    // std::rotate / move assignment (Grid.cpp); the receiving rows must all
    // come out dirty without any call-site sprinkling.
    auto lines = std::vector<Line> {};
    for (auto const text: { "aaaa", "bbbb", "cccc" })
    {
        auto line = Line(ColumnCount(4), LineFlag::None, GraphicsAttributes {});
        line.fill(ColumnOffset(0), GraphicsAttributes {}, text);
        static_cast<void>(line.stampRevision(1));
        lines.push_back(std::move(line));
    }

    std::ranges::rotate(lines, lines.begin() + 1);

    CHECK(lines[0].toUtf8() == "bbbb");
    for (auto const& line: lines)
        CHECK(line.isDirty());
}

// The renditions capture-pane reproduces come from the SgrFlagCodes table (vtbackend/SgrWriter.h),
// which is shared with the terminal's own DECRQSS report. It used to hold one int per flag, so it
// could not spell the ECMA-48 SUB-parameter forms and collapsed every underline STYLE onto plain
// `4` -- while DECRQSS, from its own private copy of the same mapping, answered them correctly. Two
// copies, one right and one wrong; these pin the survivor.
TEST_CASE("Line.toUtf8WithSgr.underlineStylesKeepTheirSubParameter", "[Line][sgr]")
{
    auto const capture = [](CellFlag flag) {
        auto line = Line(ColumnCount(2), LineFlag::None, GraphicsAttributes {});
        line.useCellAt(ColumnOffset(0)).write(GraphicsAttributes { .flags = CellFlags { flag } }, U'x', 1);
        return line.toUtf8WithSgr(ColumnOffset(0), ColumnOffset(1));
    };

    CHECK(capture(CellFlag::Underline).contains("\033[0;4m"));
    CHECK(capture(CellFlag::CurlyUnderlined).contains("\033[0;4:3m"));
    CHECK(capture(CellFlag::DottedUnderline).contains("\033[0;4:4m"));
    CHECK(capture(CellFlag::DashedUnderline).contains("\033[0;4:5m"));
    // `21` is read as "bold off" by a number of terminals; the sub-parameter form is unambiguous.
    CHECK(capture(CellFlag::DoublyUnderlined).contains("\033[0;4:2m"));
    // Framed was missing from the table entirely, so it rendered as no rendition at all.
    CHECK(capture(CellFlag::Framed).contains("\033[0;51m"));
    CHECK(capture(CellFlag::Overline).contains("\033[0;53m"));
}

TEST_CASE("Line.toUtf8WithSgr.blankLineKeepsItsFillRendition", "[Line][sgr]")
{
    // A blank line is uniformly its FILL rendition, and that is NOT necessarily the default one:
    // Screen erases with the cursor's pen, so `\e[41m\e[2J` leaves every row blank on red. Rendering
    // those rows as bare spaces silently dropped the colour of every cleared region from a
    // `capture-pane -e`, while the rows that happened to hold text kept theirs.
    auto const fill = GraphicsAttributes { .backgroundColor = IndexedColor::Red };
    auto const line = Line(ColumnCount(4), LineFlag::None, fill);
    REQUIRE(line.isBlank());

    auto const captured = line.toUtf8WithSgr(ColumnOffset(0), ColumnOffset(4));
    CHECK(captured.contains("\033[0;41m"));
    CHECK(captured.contains("    "));
    // The rendition is closed so it cannot bleed onto whatever line is replayed next — the same
    // guarantee the per-cell path gives.
    CHECK(captured.ends_with("\033[m"));
}

TEST_CASE("Line.toUtf8WithSgr.blankLineAtTheDefaultPenEmitsNoEscapes", "[Line][sgr]")
{
    // The overwhelmingly common case must stay free of escapes: an empty screen captured with -e
    // should not gain an SGR per row.
    auto const line = Line(ColumnCount(4), LineFlag::None, GraphicsAttributes {});
    REQUIRE(line.isBlank());
    CHECK(line.toUtf8WithSgr(ColumnOffset(0), ColumnOffset(4)) == "    ");
}

TEST_CASE("Line.toUtf8WithSgr.blankLineFillCarriesStyleFlagsToo", "[Line][sgr]")
{
    // `\e[7m\e[K` erases under an INVERSE pen — a rendition the row's colours alone cannot describe.
    auto const fill = GraphicsAttributes { .flags = CellFlags { CellFlag::Inverse } };
    auto const line = Line(ColumnCount(3), LineFlag::None, fill);
    REQUIRE(line.isBlank());
    CHECK(line.toUtf8WithSgr(ColumnOffset(0), ColumnOffset(3)).contains("\033[0;7m"));
}

TEST_CASE("Line.toUtf8WithSgr.carriesTheUnderlineColour", "[Line][sgr]")
{
    // The other half of the same drift: the FLAG half was consolidated into SgrFlagCodes while the
    // COLOUR half stayed duplicated, and the capture's copy knew nothing about SGR 58 — so a pane
    // using coloured underlines lost every one of them through `capture-pane -e`, while DECRQSS
    // (from the other copy) reported them. SGR 58 has no aixterm short form, so even an indexed
    // colour goes out in the `58;5;n` form.
    auto const capture = [](Color underline) {
        auto line = Line(ColumnCount(2), LineFlag::None, GraphicsAttributes {});
        line.useCellAt(ColumnOffset(0))
            .write(GraphicsAttributes { .underlineColor = underline,
                                        .flags = CellFlags { CellFlag::CurlyUnderlined } },
                   U'x',
                   1);
        return line.toUtf8WithSgr(ColumnOffset(0), ColumnOffset(1));
    };

    CHECK(capture(RGBColor { 0x11, 0x22, 0x33 }).contains("58;2;17;34;51"));
    CHECK(capture(Color::Indexed(IndexedColor::Red)).contains("58;5;1"));
    // The default underline colour is already implied by the leading reset and stays unspoken.
    CHECK_FALSE(capture(defaultColor()).contains("58"));
}

TEST_CASE("Line.isBulkWritable.refusesOnlySpanningLines", "[Line]")
{
    auto line = Line(ColumnCount(4));
    CHECK(line.isBulkWritable()); // blank

    auto& soa = line.materializedStorage();
    CHECK(line.isBulkWritable()); // Uniform
    degradeShape(soa, LineShape::Mixed);
    CHECK(line.isBulkWritable());
    degradeShape(soa, LineShape::Spanning);
    CHECK_FALSE(line.isBulkWritable());
}

TEST_CASE("Line.recycle.leavesTheSameLineAsReset", "[Line]")
{
    // Whatever the evicted line held -- colour, a hyperlink, a wide character, a grapheme cluster --
    // a recycled line must be indistinguishable from a reset one, at once and after the next write.
    auto const usedLine = [] {
        auto line = Line(ColumnCount(10), LineFlag::Wrappable);
        (void) writeTextToSoA(line.materializedStorage(),
                              0,
                              "ab\xE4\xB8\xAD"
                              "e\xCC\x81",
                              GraphicsAttributes { .foregroundColor = Color::Indexed(2) },
                              HyperlinkId(3));
        line.setFlag(LineFlag::Marked, true);
        line.setPromptEndOffset(ColumnOffset(2));
        return line;
    };
    auto const observe = [](Line const& line) {
        auto widths = std::vector<uint8_t> {};
        for (auto const column: views::iota(0, unbox<int>(line.size())))
            widths.push_back(line.cellWidthAt(ColumnOffset(column)));
        return std::tuple { line.isBlank(),
                            line.empty(),
                            line.size(),
                            line.flags(),
                            line.toUtf8(),
                            line.toUtf8WithSgr(ColumnOffset(0), boxed_cast<ColumnOffset>(line.size())),
                            line.trimmedColumns(),
                            line.isTrivialBuffer(),
                            line.isBulkWritable(),
                            line.storage().shape,
                            line.storage().fillAttrs,
                            widths,
                            line.promptEndOffset(),
                            line.commandEndOffset(),
                            line.contextId() };
    };
    auto const fill = GraphicsAttributes { .backgroundColor = Color::Indexed(4) };

    auto recycled = usedLine();
    auto reset = usedLine();
    recycled.recycle(LineFlag::Wrappable, fill, ColumnCount(10));
    reset.reset(LineFlag::Wrappable, fill, ColumnCount(10));
    CHECK(observe(recycled) == observe(reset));

    recycled.fill(ColumnOffset(2), GraphicsAttributes {}, "xyz");
    reset.fill(ColumnOffset(2), GraphicsAttributes {}, "xyz");
    CHECK(observe(recycled) == observe(reset));
}

TEST_CASE("Line.recycle.keepsTheStorageForTheNextWrite", "[Line]")
{
    auto line = Line(ColumnCount(80));
    line.fill(ColumnOffset(0), GraphicsAttributes {}, "evicted");
    auto const* const storage = line.storage().codepoints.data();

    line.recycle(LineFlag::None, GraphicsAttributes {}, ColumnCount(80));
    REQUIRE(line.isBlank());
    CHECK(line.storage().codepoints.capacity() >= 80);

    line.fill(ColumnOffset(0), GraphicsAttributes {}, "scrolled in");
    CHECK(line.storage().codepoints.data() == storage);

    // reset() still releases it, which is what clearing the screen wants.
    line.reset(LineFlag::None, GraphicsAttributes {}, ColumnCount(80));
    CHECK(line.storage().codepoints.capacity() == 0);
}

// {{{ block id and birth time

TEST_CASE("Line.block.adoptIgnoresZeroAndLeavesTheLineClean", "[Line][semanticblocks]")
{
    auto line = Line(ColumnCount(8), LineFlag::None, GraphicsAttributes {});
    static_cast<void>(line.stampRevision(1)); // clean

    CHECK(line.blockId() == CommandBlockId {});
    line.adoptBlock(CommandBlockId { 4 });
    CHECK(line.blockId() == CommandBlockId { 4 });
    // Derived state, like the context id: adopting it is not a change a mirror must be told about.
    CHECK_FALSE(line.isDirty());

    // Zero is an absence of information, not a claim: it never erases an id the line already has.
    line.adoptBlock(CommandBlockId {});
    CHECK(line.blockId() == CommandBlockId { 4 });

    // Last writer wins: the row a ;D closes is routinely the row the next ;A opens.
    line.adoptBlock(CommandBlockId { 5 });
    CHECK(line.blockId() == CommandBlockId { 5 });
}

TEST_CASE("Line.bornAt.stampsOnceAndDirtiesOnce", "[Line][semanticblocks]")
{
    auto line = Line(ColumnCount(8), LineFlag::None, GraphicsAttributes {});
    static_cast<void>(line.stampRevision(1));
    CHECK(line.bornAt() == 0);

    line.stampBornAt(0); // "no clock yet" stamps nothing
    CHECK(line.bornAt() == 0);
    CHECK_FALSE(line.isDirty());

    line.stampBornAt(0x12'34'56);
    CHECK(line.bornAt() == 0x12'34'56); // all three bytes round-trip
    CHECK(line.isDirty());              // so the daemon's delta carries the stamp

    static_cast<void>(line.stampRevision(2));
    line.stampBornAt(99); // the cursor coming back later does not make the line younger
    CHECK(line.bornAt() == 0x12'34'56);
    CHECK_FALSE(line.isDirty());
}

TEST_CASE("Line.bornAt.saturatesAtTheHorizon", "[Line][semanticblocks]")
{
    // MaxBornAt + 1 is the first stamp whose three low bytes are all zero: unclamped, it would read back as
    // "never stamped".
    for (auto const stamp: std::array { Line::MaxBornAt + 1, Line::MaxBornAt + 1000 })
    {
        auto line = Line(ColumnCount(8), LineFlag::None, GraphicsAttributes {});
        line.stampBornAt(stamp);
        CHECK(line.bornAt() == Line::MaxBornAt);
    }
}

TEST_CASE("Line.blockAndBirth.areClearedWhenTheLineIsReused", "[Line][semanticblocks]")
{
    auto const stamped = [] {
        auto line = Line(ColumnCount(8), LineFlag::None, GraphicsAttributes {});
        line.adoptBlock(CommandBlockId { 3 });
        line.stampBornAt(7);
        return line;
    };

    SECTION("reset")
    {
        auto line = stamped();
        line.reset(LineFlag::None, GraphicsAttributes {});
        CHECK(line.blockId() == CommandBlockId {});
        CHECK(line.bornAt() == 0);
    }

    SECTION("reset to a new width")
    {
        auto line = stamped();
        line.reset(LineFlag::None, GraphicsAttributes {}, ColumnCount(10));
        CHECK(line.blockId() == CommandBlockId {});
        CHECK(line.bornAt() == 0);
    }

    SECTION("recycle")
    {
        auto line = stamped();
        line.recycle(LineFlag::None, GraphicsAttributes {}, ColumnCount(8));
        CHECK(line.blockId() == CommandBlockId {});
        CHECK(line.bornAt() == 0);
    }

    SECTION("fill")
    {
        auto line = stamped();
        line.fill(LineFlag::None, GraphicsAttributes {}, U'E', 1);
        CHECK(line.blockId() == CommandBlockId {});
        CHECK(line.bornAt() == 0);
    }
}

TEST_CASE("Line.reset.aFlagItKeepsKeepsItsOffset", "[Line][semanticblocks]")
{
    // An offset means nothing without its flag, and a flag that names a border means little without its
    // offset: a reset keeps them together, or drops them together.
    auto const marked = [] {
        auto line = Line(ColumnCount(8), LineFlag::None, GraphicsAttributes {});
        line.setFlag(LineFlags { LineFlag::PromptEnd, LineFlag::CommandEnd }, true);
        line.setPromptEndOffset(ColumnOffset(2));
        line.setCommandEndOffset(ColumnOffset(5));
        return line;
    };

    SECTION("both kept")
    {
        auto line = marked();
        line.reset(line.flags(), GraphicsAttributes {});
        CHECK(line.promptEndOffset() == ColumnOffset(2));
        CHECK(line.commandEndOffset() == ColumnOffset(5));
    }

    SECTION("one kept")
    {
        auto line = marked();
        line.reset(LineFlag::PromptEnd, GraphicsAttributes {});
        CHECK(line.promptEndOffset() == ColumnOffset(2));
        CHECK(line.commandEndOffset() == ColumnOffset(0));
    }

    SECTION("none kept")
    {
        auto line = marked();
        line.reset(LineFlag::None, GraphicsAttributes {});
        CHECK(line.promptEndOffset() == ColumnOffset(0));
        CHECK(line.commandEndOffset() == ColumnOffset(0));
    }

    SECTION("a flag the row did not carry starts with no offset")
    {
        auto line = Line(ColumnCount(8), LineFlag::None, GraphicsAttributes {});
        line.setPromptEndOffset(ColumnOffset(4)); // a stale column, with no flag to give it meaning
        line.reset(LineFlag::PromptEnd, GraphicsAttributes {});
        CHECK(line.promptEndOffset() == ColumnOffset(0));
    }
}

TEST_CASE("Line.blockAndBirth.travelWithTheLineOnAssignment", "[Line][semanticblocks]")
{
    // Margin scrolls move whole lines between rows by assignment: the row that receives the text must
    // receive its block and its birth time with it.
    auto source = Line(ColumnCount(4), LineFlag::None, GraphicsAttributes {});
    source.adoptBlock(CommandBlockId { 9 });
    source.stampBornAt(11);

    auto copied = Line(ColumnCount(4), LineFlag::None, GraphicsAttributes {});
    copied = source;
    CHECK(copied.blockId() == CommandBlockId { 9 });
    CHECK(copied.bornAt() == 11);

    auto moved = Line(ColumnCount(4), LineFlag::None, GraphicsAttributes {});
    moved = std::move(source);
    CHECK(moved.blockId() == CommandBlockId { 9 });
    CHECK(moved.bornAt() == 11);
}

TEST_CASE("Line.provenance.isWhatAFreshLineAdopts", "[Line][semanticblocks]")
{
    // Reflow cuts a logical line into fresh rows and hands each the provenance of the row it came from.
    auto source = Line(ColumnCount(8), LineFlag::None, GraphicsAttributes {});
    source.adoptContext(ContextId { 5 });
    source.adoptBlock(CommandBlockId { 9 });
    source.stampBornAt(11);

    auto piece = Line(ColumnCount(4), LineFlag::None, GraphicsAttributes {});
    piece.adoptProvenance(source.provenance());
    CHECK(piece.contextId() == ContextId { 5 });
    CHECK(piece.blockId() == CommandBlockId { 9 });
    CHECK(piece.bornAt() == 11);
}

TEST_CASE("Line.provenance.fillsOnlyWhatTheRowDoesNotCarryYet", "[Line][semanticblocks]")
{
    // The cursor reaching a row hands it what is in effect only where the row carries nothing yet: a writer,
    // a block or a time it already has is never relabelled.
    auto line = Line(ColumnCount(8), LineFlag::None, GraphicsAttributes {});
    line.adoptContext(ContextId { 3 });
    line.stampBornAt(7);

    line.adoptProvenance(
        LineProvenance { .contextId = ContextId { 5 }, .blockId = CommandBlockId { 9 }, .bornAt = 11 });

    CHECK(line.contextId() == ContextId { 3 });
    CHECK(line.blockId() == CommandBlockId { 9 }); // the one part it had none of
    CHECK(line.bornAt() == 7);

    line.adoptProvenance(LineProvenance { .contextId = {}, .blockId = CommandBlockId { 12 }, .bornAt = 0 });
    CHECK(line.blockId() == CommandBlockId { 9 });
}

TEST_CASE("Line.provenance.aRelabelThatChangesNothingIsKnownBeforehand", "[Line][semanticblocks]")
{
    // A transition is fanned out to every screen, and handing a scrollback row out for writing costs a deeper
    // delta scan whether it then changes or not: a row the transition leaves as it is says so up front.
    auto line = Line(ColumnCount(8), LineFlag::None, GraphicsAttributes {});
    line.adoptContext(ContextId { 3 });
    line.adoptBlock(CommandBlockId { 9 });
    auto const relabelling = [](ContextId context, CommandBlockId block) {
        return LineProvenance { .contextId = context, .blockId = block, .bornAt = 0 };
    };

    CHECK_FALSE(line.wouldRelabel(relabelling({}, {})));
    CHECK_FALSE(line.wouldRelabel(relabelling(ContextId { 3 }, {})));
    CHECK_FALSE(line.wouldRelabel(relabelling({}, CommandBlockId { 9 })));
    CHECK_FALSE(line.wouldRelabel(relabelling(ContextId { 3 }, CommandBlockId { 9 })));
    CHECK_FALSE(line.wouldRelabel(LineProvenance { .contextId = {}, .blockId = {}, .bornAt = 42 })); // kept
    CHECK(line.wouldRelabel(relabelling(ContextId { 4 }, {})));
    CHECK(line.wouldRelabel(relabelling({}, CommandBlockId { 10 })));
    CHECK(line.wouldRelabel(relabelling(ContextId { 3 }, CommandBlockId { 10 })));

    // ...and relabel() dirties the row exactly when it said so.
    line.stampRevision(1);
    line.relabel(relabelling(ContextId { 3 }, CommandBlockId { 9 }));
    CHECK_FALSE(line.isDirty());
    line.relabel(relabelling({}, CommandBlockId { 10 }));
    CHECK(line.isDirty());
    CHECK(line.blockId() == CommandBlockId { 10 });
    CHECK(line.contextId() == ContextId { 3 });
}

// }}}

TEST_CASE("LineFlags.userMarkIsAHeadOnlyNavigationMark", "[Line][semanticblocks]")
{
    CHECK(std::format("{}", LineFlags { LineFlag::UserMark }) == "UserMark");
    CHECK(HeadOnlyLineFlags.contains(LineFlag::UserMark));
    CHECK(NavigationMarkFlags == LineFlags { LineFlag::Marked, LineFlag::UserMark });
}

TEST_CASE("LineFlags.theMarkSetsFollowTheTablesRoles", "[Line][semanticblocks]")
{
    // A reader that means "the shell spoke here" must not see the user's bookmark.
    CHECK(
        ShellMarkLineFlags
        == LineFlags { LineFlag::Marked, LineFlag::OutputStart, LineFlag::CommandEnd, LineFlag::PromptEnd });
    CHECK(HeadOnlyLineFlags == ShellMarkLineFlags.with(LineFlag::UserMark));
    CHECK(lineFlagsOf(LineFlagRole::Physical) == LineFlags { LineFlag::Wrappable, LineFlag::Wrapped });
    CHECK(LineRenditionFlags
          == LineFlags { LineFlag::DoubleWidth, LineFlag::DoubleHeightTop, LineFlag::DoubleHeightBottom });
}
