// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/grid/Line.hpp>

#include <crispy/Escape.hpp>

#include <catch2/catch_test_macros.hpp>

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

TEST_CASE("Line.blank.searchOnlyMatchesEmpty", "[Line][blank]")
{
    auto line = Line(ColumnCount(20), LineFlag::None, GraphicsAttributes {});

    REQUIRE(line.isBlank());
    CHECK(line.search(U"hello", ColumnOffset(0), true) == std::nullopt);
    CHECK(line.searchReverse(U"hello", ColumnOffset(19), true) == std::nullopt);
    auto const empty = line.search(U"", ColumnOffset(0), true);
    REQUIRE(empty.has_value());
    CHECK(empty->column == ColumnOffset(0));
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
