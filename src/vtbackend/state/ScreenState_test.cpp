// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/state/ScreenState.hpp>
#include <vtbackend/state/StateFields.hpp>
#include <vtbackend/testing/MockTerm.hpp>
#include <vtbackend/testing/Workloads.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

using namespace vtbackend;
using namespace std::string_view_literals;

namespace
{
using Mock = MockTerm<vtpty::MockPty>;

constexpr auto TestPage = PageSize { LineCount(6), ColumnCount(20) };
constexpr auto TestHistory = LineCount(10);

std::unique_ptr<Mock> makeTerminal()
{
    auto mock = std::make_unique<Mock>(TestPage, TestHistory);
    mock->terminal.setCellPixelSize(ImageSize { Width(10), Height(20) });
    return mock;
}

/// What a capture of the primary screen is, after the byte codec: the screen and its image table.
struct Captured
{
    ScreenState screen;
    ImageTableState table;
};

Captured capture(Mock& mock)
{
    auto builder = ImageTableBuilder { mock.terminal.imagePool() };
    auto screen = mock.terminal.primaryScreen().captureState(builder);
    return Captured { .screen = std::move(screen), .table = std::move(builder).finish() };
}

/// @return @p value in the byte codec.
template <typename T>
std::vector<uint8_t> bytesOf(T const& value)
{
    auto writer = StateWriter::payload();
    detail::encodeFields(writer, value);
    return std::move(writer).take();
}

/// @return @p value through the byte codec.
template <typename T>
T throughCodec(T const& value)
{
    auto decoded = T {};
    REQUIRE(detail::decodeFields(bytesOf(value), decoded).has_value());
    return decoded;
}

/// The bytes a capture of the primary screen encodes to: the identity two screens are the same state by.
std::vector<uint8_t> encoded(Captured const& captured)
{
    auto bytes = bytesOf(captured.table);
    auto const screen = bytesOf(captured.screen);
    bytes.insert(bytes.end(), screen.begin(), screen.end());
    return bytes;
}

/// Sends a capture through the byte codec and back.
Captured throughBytes(Captured const& captured)
{
    return Captured { .screen = throughCodec(captured.screen), .table = throughCodec(captured.table) };
}

/// Restores @p captured into the primary screen of @p target; the result keeps its images alive.
RestoredImageTable restoreInto(Mock& target, Captured const& captured)
{
    auto restored = RestoredImageTable::build(target.terminal.imagePool(), captured.table);
    REQUIRE(restored.has_value());
    restored->commit(target.terminal.imagePool());
    REQUIRE(target.terminal.primaryScreen().restoreState(captured.screen, *restored).has_value());
    return *std::move(restored);
}

/// A terminal that is @p source restored into a fresh one, through the byte codec.
struct Restored
{
    std::unique_ptr<Mock> mock;
    RestoredImageTable images;
};

Restored restoredCopyOf(Mock& source)
{
    auto target = makeTerminal();
    auto images = restoreInto(*target, throughBytes(capture(source)));
    return Restored { .mock = std::move(target), .images = std::move(images) };
}

/// Feeds @p suffix to the source and its restored copy, and checks they are still the same screen.
void requireSameAfter(Mock& source, Mock& restored, std::string_view suffix)
{
    source.writeToScreen(suffix);
    restored.writeToScreen(suffix);
    CHECK(source.terminal.primaryScreen().renderMainPageText()
          == restored.terminal.primaryScreen().renderMainPageText());
    CHECK(encoded(capture(source)) == encoded(capture(restored)));
}
} // namespace

TEST_CASE("DEC special graphics designated into G1 and shifted in survives", "[ScreenState]")
{
    auto source = makeTerminal();
    source->writeToScreen("ab\033)0\x0E"sv); // G1 = DEC special graphics, SO: G1 into GL.
    auto restored = restoredCopyOf(*source);

    CHECK(restored.mock->terminal.primaryScreen().cursor().charsets.captureState()
          == source->terminal.primaryScreen().cursor().charsets.captureState());
    requireSameAfter(*source, *restored.mock, "qqx");
    // The line-drawing glyphs, not the letters, were written: 'q' maps to U+2500.
    CHECK(restored.mock->terminal.primaryScreen().at(LineOffset(0), ColumnOffset(2)).codepoint(0) == U'─');
    CHECK(restored.mock->terminal.primaryScreen().at(LineOffset(0), ColumnOffset(4)).codepoint(0) == U'│');
}

TEST_CASE("a single shift, DRCS and 96-character designations survive", "[ScreenState]")
{
    auto source = makeTerminal();
    source->writeToScreen("\033*0\033+A\033N"sv); // G2 = special, G3 = British (96-set position), SS2.
    auto restored = restoredCopyOf(*source);
    CHECK(restored.mock->terminal.primaryScreen().cursor().charsets.captureState()
          == source->terminal.primaryScreen().cursor().charsets.captureState());
    requireSameAfter(*source, *restored.mock, "qq");
}

TEST_CASE("saved cursor with SGR and origin mode survives DECRC after restore", "[ScreenState]")
{
    auto source = makeTerminal();
    source->writeToScreen("\033[2;5r"sv);         // Top/bottom margins, so origin mode has a meaning.
    source->writeToScreen("\033[?6h\033[2;3H"sv); // DECOM, cursor to row 2 column 3 of the margin.
    source->writeToScreen("\033[1;31m\033)0\033[4m"sv);
    source->writeToScreen("\0337"sv); // DECSC
    source->writeToScreen("\033[0m\033[?6l\033[H\033)B"sv);
    auto restored = restoredCopyOf(*source);

    requireSameAfter(*source, *restored.mock, "\0338Z"sv); // DECRC, then a letter
    auto const& cell = restored.mock->terminal.primaryScreen().at(
        restored.mock->terminal.primaryScreen().cursor().position.line, ColumnOffset(2));
    CHECK(cell.flags().contains(CellFlag::Bold));
    CHECK(cell.flags().contains(CellFlag::Underline));
    CHECK(cell.foregroundColor() == Color { IndexedColor::Red });
}

TEST_CASE("OSC 133 live prompt marks survive", "[ScreenState]")
{
    auto source = makeTerminal();
    source->writeToScreen("\033]133;A\033\\$ \033]133;B\033\\ls\033]133;P;k=r\033\\ RP\033]133;I\033\\"sv);
    auto const marks = source->terminal.primaryScreen()
                           .captureState(*std::make_unique<ImageTableBuilder>(source->terminal.imagePool()))
                           .livePromptMarks;
    REQUIRE_FALSE(marks.empty());

    auto restored = restoredCopyOf(*source);
    auto builder = ImageTableBuilder { restored.mock->terminal.imagePool() };
    CHECK(restored.mock->terminal.primaryScreen().captureState(builder).livePromptMarks == marks);
    // The marks are read when the command starts: both sides recover the same command line.
    requireSameAfter(*source, *restored.mock, "\033]133;C\033\\out\r\n"sv);
}

TEST_CASE("a ReGIS canvas drawn before the switch is extended, not redrawn, after it", "[ScreenState][regis]")
{
    auto source = makeTerminal();
    source->writeToScreen("\033Pp P[10,10]V[400,300] \033\\"sv);
    auto const before = capture(*source);
    REQUIRE(before.screen.regis.has_value());
    auto const painted = std::ranges::count_if(before.screen.regis->pixels, [](uint8_t b) { return b != 0; });
    REQUIRE(painted > 0);

    auto restored = restoredCopyOf(*source);
    auto const afterRestore = capture(*restored.mock);
    REQUIRE(afterRestore.screen.regis.has_value());
    CHECK(afterRestore.screen.regis->pixels == before.screen.regis->pixels);
    CHECK(afterRestore.screen.regis->context.position.x == before.screen.regis->context.position.x);

    // A second string draws on top of what the first drew -- on both sides, to the same pixels.
    source->writeToScreen("\033Pp V[100,100] \033\\"sv);
    restored.mock->writeToScreen("\033Pp V[100,100] \033\\"sv);
    auto const sourceAfter = capture(*source);
    auto const restoredAfter = capture(*restored.mock);
    REQUIRE(sourceAfter.screen.regis.has_value());
    REQUIRE(restoredAfter.screen.regis.has_value());
    CHECK(restoredAfter.screen.regis->pixels == sourceAfter.screen.regis->pixels);
    CHECK(std::ranges::count_if(restoredAfter.screen.regis->pixels, [](uint8_t b) { return b != 0; })
          > painted);
}

TEST_CASE("the ReGIS state and every screen scalar round-trip through the codec", "[ScreenState]")
{
    auto source = makeTerminal();
    source->writeToScreen("\033Pp P[10,10]W(I3)V[40,30] \033\\"sv);
    source->writeToScreen("\033[?1+q"sv); // Nothing observable; just keeps the stream non-trivial.
    auto captured = capture(*source);
    REQUIRE(captured.screen.regis.has_value());
    captured.screen.regis->context.position.x = 77;
    captured.screen.enableLocalFunctions = 3;
    captured.screen.localFunctionKeyControl = 4;
    captured.screen.modifierKeyReporting = 5;
    captured.screen.rectangularAttributeMode = false;
    captured.screen.isoProtectionActive = true;

    auto const decoded = throughBytes(captured);
    CHECK(encoded(decoded) == encoded(captured));
    REQUIRE(decoded.screen.regis.has_value());
    CHECK(decoded.screen.regis->context.position.x == 77);
    CHECK(decoded.screen.enableLocalFunctions == 3);
    CHECK(decoded.screen.localFunctionKeyControl == 4);
    CHECK(decoded.screen.modifierKeyReporting == 5);
    CHECK_FALSE(decoded.screen.rectangularAttributeMode);
    CHECK(decoded.screen.isoProtectionActive);

    auto target = makeTerminal();
    auto const images = restoreInto(*target, decoded);
    CHECK(encoded(capture(*target)) == encoded(decoded));
}

TEST_CASE("a state that does not fit the screen is refused", "[ScreenState]")
{
    auto source = makeTerminal();
    source->writeToScreen("hello"sv);
    auto const good = capture(*source);

    auto const refused = [&](auto&& tamper) {
        auto bad = good;
        tamper(bad.screen);
        auto target = makeTerminal();
        auto images = RestoredImageTable::build(target->terminal.imagePool(), bad.table);
        REQUIRE(images.has_value());
        return target->terminal.primaryScreen().restoreState(bad.screen, *images);
    };

    auto const offPage =
        refused([](ScreenState& s) { s.cursor.position.line = LineOffset(TestPage.lines.value); });
    REQUIRE_FALSE(offPage.has_value());
    CHECK(offPage.error() == StateDecodeError::InvalidValue);

    auto const badCharset =
        refused([](ScreenState& s) { s.cursor.charsets.charsetIds[1] = static_cast<CharsetId>(99); });
    REQUIRE_FALSE(badCharset.has_value());

    auto const badSavedCursor =
        refused([](ScreenState& s) { s.savedCursor.position.column = ColumnOffset(-1); });
    REQUIRE_FALSE(badSavedCursor.has_value());

    auto const badCanvas = refused([](ScreenState& s) {
        s.regis = regis::ReGISState {};
        s.regis->canvasSize = ImageSize { Width(2), Height(2) };
    });
    REQUIRE_FALSE(badCanvas.has_value());

    auto const wrongSize =
        refused([](ScreenState& s) { s.grid.pageSize = PageSize { LineCount(3), ColumnCount(3) }; });
    REQUIRE_FALSE(wrongSize.has_value());
}

TEST_CASE("a truncated screen record is refused by the codec", "[ScreenState]")
{
    auto source = makeTerminal();
    source->writeToScreen("hello"sv);
    auto bytes = bytesOf(capture(*source).screen);
    bytes.resize(bytes.size() - 3);
    auto decoded = ScreenState {};
    CHECK_FALSE(detail::decodeFields(bytes, decoded).has_value());
}

TEST_CASE("a screen restored at every line boundary of the Workloads corpus keeps behaving identically",
          "[ScreenState][StateRoundTrip]")
{
    // Cut after a line feed: no escape sequence is in flight there (the parser's tail is Terminal state), and
    // the workloads set no terminal-level state that the screen alone would have to carry.
    constexpr auto CorpusBytes = size_t { 12 } * 1024;
    constexpr auto Stride = size_t { 3 } * 1024;
    constexpr auto Page =
        PageSize { LineCount(test::WorkloadPageLines), ColumnCount(test::WorkloadPageColumns) };
    for (auto const& workload: test::Workloads)
    {
        if (workload.name == "tui")
            continue; // Addresses the cursor with margins and modes, which belong to the terminal.
        DYNAMIC_SECTION(workload.name)
        {
            auto const corpus = test::makeWorkload(workload, CorpusBytes);
            auto const cutAfterLineFeed = [&](size_t target) {
                auto const lineFeed = corpus.find('\n', std::min(target, corpus.size()));
                return lineFeed == std::string::npos ? corpus.size() : lineFeed + 1;
            };
            auto source = std::make_unique<Mock>(Page, LineCount(100));
            auto restored = std::unique_ptr<Mock> {}; // Restored at the previous cut, one chunk behind.
            auto keepAlive = std::optional<RestoredImageTable> {};
            auto from = size_t { 0 };
            while (from < corpus.size())
            {
                auto const to = cutAfterLineFeed(from + Stride);
                auto const chunk = std::string_view { corpus }.substr(from, to - from);
                source->writeToScreen(chunk);
                if (restored)
                {
                    // Restored before this chunk and fed it since: the same screen as the source.
                    REQUIRE(source->terminal.primaryScreen().renderMainPageText()
                            == restored->terminal.primaryScreen().renderMainPageText());
                    REQUIRE(encoded(capture(*source)) == encoded(capture(*restored)));
                }
                restored = std::make_unique<Mock>(Page, LineCount(100));
                auto const captured = throughBytes(capture(*source));
                keepAlive = restoreInto(*restored, captured);
                REQUIRE(encoded(capture(*restored)) == encoded(captured));
                auto const next = cutAfterLineFeed(to + Stride);
                restored->writeToScreen(std::string_view { corpus }.substr(to, next - to));
                from = to;
            }
        }
    }
}
