// SPDX-License-Identifier: Apache-2.0
//
// The extension's private terminal: what it shows after a frame, what it answers, and the bounds it keeps.
#include <vtbackend/screen/StatusLineExtensions.hpp>
#include <vtbackend/screen/Terminal.hpp>
#include <vtbackend/testing/MockTerm.hpp>

#include <core/Base64.hpp>
#include <core/platform/Clock.hpp>
#include <core/platform/testing/InMemoryFileSystem.hpp>
#include <core/testing/Environment.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>

#include <vtextension/ExtensionLimits.hpp>
#include <vtextension/Surface.hpp>

using namespace std::string_view_literals;
using vtbackend::CellFlag;
using vtbackend::ColumnCount;
using vtbackend::ColumnOffset;
using vtbackend::Height;
using vtbackend::ImageSize;
using vtbackend::IndexedColor;
using vtbackend::LineCount;
using vtbackend::PageSize;
using vtbackend::Width;
using vtextension::Surface;
using vtextension::SurfaceConfig;

namespace
{

/// The cell size every surface here is built with.
constexpr auto CellSize = ImageSize { Width(8), Height(16) };

/// A surface over fake ambient resources: no environment, an empty filesystem, clocks at their epoch.
class SurfaceFixture
{
  public:
    explicit SurfaceFixture(int columns,
                            std::size_t replyCapacity = vtextension::ExtensionLimits {}.maxReplyBytes):
        surface { SurfaceConfig { .pageSize =
                                      PageSize { .lines = LineCount(1), .columns = ColumnCount(columns) },
                                  .cellPixelSize = CellSize,
                                  .replyCapacity = replyCapacity },
                  vtextension::SurfaceServices {
                      .environment = _environment,
                      .fileSystem = _fileSystem,
                      .clocks = vtbackend::TerminalClocks {
                          .steady = _steadyClock, .wall = _wallClock, .localTime = &utcLocalTime } } }
    {
    }

  private:
    /// Reads every instant as UTC, so no test depends on the zone of the machine it runs on.
    static std::chrono::local_seconds utcLocalTime(std::chrono::system_clock::time_point when) noexcept
    {
        return std::chrono::local_seconds {
            std::chrono::floor<std::chrono::seconds>(when).time_since_epoch()
        };
    }

    // Declared before `surface`, which borrows them for its life.
    core::testing::FakeEnvironment _environment;
    core::platform::testing::InMemoryFileSystem _fileSystem;
    core::platform::ManualClock _steadyClock;
    core::platform::ManualWallClock _wallClock;

  public:
    Surface surface;
};

/// Where visibleText() is within an escape sequence.
enum class Lexing : uint8_t
{
    Text,   ///< Outside any sequence.
    Escape, ///< After ESC.
    Csi,    ///< Inside a CSI sequence, before its final byte.
};

/// @return @p text with every escape sequence removed: what a reader sees of it.
std::string visibleText(std::string_view text)
{
    auto result = std::string {};
    auto state = Lexing::Text;
    for (auto const ch: text)
    {
        switch (state)
        {
            case Lexing::Text:
                if (ch == '\033')
                    state = Lexing::Escape;
                else
                    result += ch;
                break;
            case Lexing::Escape: state = ch == '[' ? Lexing::Csi : Lexing::Text; break;
            case Lexing::Csi:
                if (ch >= 0x40 && ch <= 0x7E)
                    state = Lexing::Text;
                break;
        }
    }
    return result;
}

} // namespace

TEST_CASE("Surface.sgr", "[surface]")
{
    auto fixture = SurfaceFixture { 20 };
    fixture.surface.feed("\033[1;31mERR\033[m ok");

    auto snapshot = fixture.surface.snapshot(7);
    CHECK(snapshot.generation == 7);
    REQUIRE(snapshot.lines.size() == 1);
    auto& line = snapshot.lines[0];
    CHECK(line.toUtf8Trimmed() == "ERR ok");
    auto const cell = line.useCellAt(ColumnOffset(0));
    CHECK(cell.flags().contains(CellFlag::Bold));
    CHECK(cell.foregroundColor() == IndexedColor::Red);
    CHECK_FALSE(line.useCellAt(ColumnOffset(4)).flags().contains(CellFlag::Bold));

    CHECK(visibleText(snapshot.sgrText) == "ERR ok");

    // Round trip: the serialized row, fed into a fresh surface, shows the same cells.
    auto replay = SurfaceFixture { 20 };
    replay.surface.feed(snapshot.sgrText);
    auto replayed = replay.surface.snapshot(8);
    REQUIRE(replayed.lines.size() == 1);
    for (auto const column: std::views::iota(0, 20))
    {
        INFO("column " << column);
        auto const original = line.useCellAt(ColumnOffset(column));
        auto const copy = replayed.lines[0].useCellAt(ColumnOffset(column));
        CHECK(copy.toUtf8() == original.toUtf8());
        CHECK(copy.graphicsAttributes() == original.graphicsAttributes());
    }
}

TEST_CASE("Surface.fullFrame", "[surface]")
{
    // Every feed is a frame of its own: neither the text nor the pen of the last one survives.
    auto fixture = SurfaceFixture { 20 };
    fixture.surface.feed("\033[41mlonger frame text");
    fixture.surface.feed("new");

    auto snapshot = fixture.surface.snapshot(1);
    REQUIRE(snapshot.lines.size() == 1);
    CHECK(snapshot.lines[0].toUtf8Trimmed() == "new");
    CHECK(vtbackend::isDefaultColor(snapshot.lines[0].useCellAt(ColumnOffset(0)).backgroundColor()));
    CHECK(snapshot.sgrText == "new");
}

TEST_CASE("Surface.reply.csi14t", "[surface]")
{
    auto fixture = SurfaceFixture { 40 };
    fixture.surface.feed("\033[14t");
    CHECK(fixture.surface.drainReplies() == "\033[4;16;320t");
    CHECK(fixture.surface.drainReplies().empty());
}

TEST_CASE("Surface.reply.da1", "[surface]")
{
    auto fixture = SurfaceFixture { 20 };
    fixture.surface.feed("\033[c");
    auto const reply = fixture.surface.drainReplies();
    CHECK(reply.starts_with("\033[?"));
    CHECK(reply.ends_with("c"));
}

TEST_CASE("Surface.reply.bounded", "[surface]")
{
    // Each feed writes back one reply; the buffer holds as many whole replies as fit, newest kept.
    auto fixture = SurfaceFixture { 20 };
    auto constexpr Queries = std::size_t { 10'000 };
    for ([[maybe_unused]] auto const _: std::views::iota(std::size_t { 0 }, Queries))
        fixture.surface.feed("\033[6n");

    auto constexpr Reply = "\033[1;1R"sv;
    auto constexpr Kept = vtextension::ExtensionLimits {}.maxReplyBytes / Reply.size(); // 682 replies
    auto const replies = fixture.surface.drainReplies();
    CHECK(replies.size() == 4092);
    CHECK(replies.size() == Kept * Reply.size());
    CHECK(replies.starts_with("\033["));
    auto wholeReplies = true;
    for (auto const index: std::views::iota(std::size_t { 0 }, Kept))
        wholeReplies = wholeReplies && replies.substr(index * Reply.size(), Reply.size()) == Reply;
    CHECK(wholeReplies);
    CHECK(fixture.surface.droppedReplies() == Queries - Kept);
    CHECK(fixture.surface.droppedReplyBytes() == (Queries - Kept) * Reply.size());
    CHECK(fixture.surface.drainReplies().empty());
}

TEST_CASE("Surface.reply.wholeChunks", "[surface]")
{
    // Capacity for one cursor report and a bit: a reply is never cut to fit.
    auto fixture = SurfaceFixture { 20, 8 };

    SECTION("the oldest reply is evicted whole to make room")
    {
        fixture.surface.feed("\033[6n");
        fixture.surface.feed("\033[1;5H\033[6n");
        CHECK(fixture.surface.drainReplies() == "\033[1;5R");
        CHECK(fixture.surface.droppedReplies() == 1);
        CHECK(fixture.surface.droppedReplyBytes() == 6);
    }

    SECTION("a reply larger than the whole buffer is dropped, the ones held stay")
    {
        fixture.surface.feed("\033[6n");
        fixture.surface.feed("\033[c"); // the DA1 reply is longer than 8 bytes
        CHECK(fixture.surface.drainReplies() == "\033[1;1R");
        CHECK(fixture.surface.droppedReplies() == 1);
        CHECK(fixture.surface.droppedReplyBytes() > 8);
    }
}

TEST_CASE("Surface.frameIsolation", "[surface]")
{
    // A frame that sets left/right margins, origin mode, insert mode, autowrap and a pen, and stops inside
    // a CSI, leaves nothing behind for the next one.
    auto fixture = SurfaceFixture { 10 };
    fixture.surface.feed("\033[?69h\033[3;8s\033[?6h\033[4h\033[?7h\033[31mabc\033[12");
    fixture.surface.feed("0123456789XYZ\033[1;1HAB");

    auto snapshot = fixture.surface.snapshot(1);
    REQUIRE(snapshot.lines.size() == 1);
    // Not swallowed by the open CSI, not clipped by a margin, not wrapped, homed to column 1 rather
    // than the margin, and overwritten rather than shifted by IRM.
    CHECK(snapshot.lines[0].toUtf8() == "AB2345678Z");
    CHECK(vtbackend::isDefaultColor(snapshot.lines[0].useCellAt(ColumnOffset(0)).foregroundColor()));
    CHECK(snapshot.sgrText == "AB2345678Z");

    // The cursor position report is not offset by origin mode either.
    fixture.surface.feed("\033[1;3H\033[6n");
    CHECK(fixture.surface.drainReplies() == "\033[1;3R");
}

TEST_CASE("Surface.frameIsolation.conformanceLevel", "[surface]")
{
    // A frame that changes what DECSTR covers does not weaken the next frame's reset.
    auto fixture = SurfaceFixture { 40 };

    SECTION("8-bit replies (S8C1T), which DECSTR leaves selected")
    {
        fixture.surface.feed("\033 Gstale");
        fixture.surface.feed("fresh\033[c\033[14t");

        auto snapshot = fixture.surface.snapshot(1);
        REQUIRE(snapshot.lines.size() == 1);
        CHECK(snapshot.lines[0].toUtf8Trimmed() == "fresh");

        auto const replies = fixture.surface.drainReplies();
        CHECK(replies.starts_with("\033[?")); // 7-bit CSI, not 0x9B
        CHECK_FALSE(replies.contains('\x9B'));
        CHECK(replies.ends_with("\033[4;16;320t"));
    }

    SECTION("the VT100 level, at which DECSTR is not honoured")
    {
        // Insert mode set after dropping to VT100 would survive a DECSTR-only reset.
        fixture.surface.feed("\033[61\"p\033[4hstale");
        fixture.surface.feed("0123456789\033[1;1HAB\033[c");

        auto snapshot = fixture.surface.snapshot(1);
        REQUIRE(snapshot.lines.size() == 1);
        CHECK(snapshot.lines[0].toUtf8Trimmed() == "AB23456789"); // overwritten, not shifted by IRM

        // DA1 reports the terminal's identity (VT525), whatever the operating level; it is checked only
        // to see the reply arrive, 7-bit.
        CHECK(fixture.surface.drainReplies().starts_with("\033[?65"));
    }
}

TEST_CASE("Surface.sixel", "[surface]")
{
    auto fixture = SurfaceFixture { 10 };
    // 16 by 6 pixels of red: two cells of the first row.
    fixture.surface.feed("\033Pq#0;2;100;0;0#0!16~\033\\");

    auto snapshot = fixture.surface.snapshot(1);
    REQUIRE(snapshot.lines.size() == 1);
    CHECK(snapshot.lines[0].useCellAt(ColumnOffset(0)).imageFragment() != nullptr);
    CHECK(snapshot.lines[0].useCellAt(ColumnOffset(1)).imageFragment() != nullptr);
    CHECK(snapshot.lines[0].useCellAt(ColumnOffset(2)).imageFragment() == nullptr);
    CHECK_FALSE(snapshot.sgrText.contains('\033'));
    CHECK(snapshot.sgrText.empty());
}

TEST_CASE("Surface.sixel.tall", "[surface]")
{
    // 16 by 48 pixels, three rows' worth on a one-row surface: the row keeps the image's top and the
    // text beside it, rather than scrolling them away.
    auto fixture = SurfaceFixture { 10 };
    fixture.surface.feed("\033Pq#0;2;100;0;0#0!16~-#0!16~-#0!16~-#0!16~-#0!16~-#0!16~-#0!16~-#0!16~\033\\"
                         "\033[1;5Hok");

    auto snapshot = fixture.surface.snapshot(1);
    REQUIRE(snapshot.lines.size() == 1);
    CHECK(snapshot.lines[0].useCellAt(ColumnOffset(0)).imageFragment() != nullptr);
    CHECK(snapshot.lines[0].useCellAt(ColumnOffset(1)).imageFragment() != nullptr);
    CHECK(snapshot.lines[0].toUtf8Trimmed() == "ok");
    CHECK(snapshot.sgrText == "    ok");
}

TEST_CASE("Surface.sixel.betweenText", "[surface]")
{
    // Text, then an image, then text again, in one row. Sixel display mode places the image at the
    // page's top-left, so it is drawn first, six cells wide, and the text overwrites its outer cells.
    auto fixture = SurfaceFixture { 10 };
    fixture.surface.feed("\033Pq#0;2;100;0;0#0!48~\033\\"
                         "\033[1;1H\033[1mab\033[m\033[1;5Hcd");

    auto snapshot = fixture.surface.snapshot(1);
    REQUIRE(snapshot.lines.size() == 1);
    auto& line = snapshot.lines[0];
    CHECK(line.useCellAt(ColumnOffset(0)).imageFragment() == nullptr);
    CHECK(line.useCellAt(ColumnOffset(2)).imageFragment() != nullptr);
    CHECK(line.useCellAt(ColumnOffset(3)).imageFragment() != nullptr);
    CHECK(line.useCellAt(ColumnOffset(4)).imageFragment() == nullptr);
    CHECK(visibleText(snapshot.sgrText) == "ab  cd");
    CHECK(snapshot.sgrText.contains("\033[")); // ab keeps its bold
}

TEST_CASE("Surface.kitty.atColumn", "[surface]")
{
    // A kitty graphics image is placed at the cursor, so unlike a Sixel it can sit anywhere in the row.
    // Two cells wide, at column 3; the placement advances the cursor past it, so the text follows it.
    auto fixture = SurfaceFixture { 10 };
    auto pixels = std::string {};
    for ([[maybe_unused]] auto const _: std::views::iota(0, 16 * 16))
        pixels += "\xFF\x00\x00\xFF"sv;
    fixture.surface.feed(
        std::format("ab\033_Ga=T,f=32,s=16,v=16,q=2;{}\033\\cd", core::base64::encode(pixels)));

    auto snapshot = fixture.surface.snapshot(1);
    REQUIRE(snapshot.lines.size() == 1);
    auto& line = snapshot.lines[0];
    CHECK(line.useCellAt(ColumnOffset(1)).imageFragment() == nullptr);
    CHECK(line.useCellAt(ColumnOffset(2)).imageFragment() != nullptr);
    CHECK(line.useCellAt(ColumnOffset(3)).imageFragment() != nullptr);
    CHECK(line.useCellAt(ColumnOffset(4)).imageFragment() == nullptr);
    CHECK(snapshot.sgrText == "ab  cd");
    CHECK(fixture.surface.drainReplies().empty());
}

TEST_CASE("Surface.kitty.atRightEdge", "[surface]")
{
    // The surface runs with DECAWM reset, so an image ending on the last column clamps the cursor
    // there instead of wrapping: on a one-line page a wrap would scroll the row -- text and image --
    // away.
    auto fixture = SurfaceFixture { 10 };
    auto pixels = std::string {};
    for ([[maybe_unused]] auto const _: std::views::iota(0, 16 * 16))
        pixels += "\xFF\x00\x00\xFF"sv;
    fixture.surface.feed(
        std::format("abcdefgh\033_Ga=T,f=32,s=16,v=16,q=2;{}\033\\", core::base64::encode(pixels)));

    auto snapshot = fixture.surface.snapshot(1);
    REQUIRE(snapshot.lines.size() == 1);
    auto& line = snapshot.lines[0];
    CHECK(line.useCellAt(ColumnOffset(7)).imageFragment() == nullptr);
    CHECK(line.useCellAt(ColumnOffset(8)).imageFragment() != nullptr);
    CHECK(line.useCellAt(ColumnOffset(9)).imageFragment() != nullptr);
    CHECK(snapshot.sgrText.starts_with("abcdefgh"));
    CHECK(fixture.surface.drainReplies().empty());
}

TEST_CASE("Surface.truncate", "[surface]")
{
    auto fixture = SurfaceFixture { 10 };
    fixture.surface.feed("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMN");

    auto snapshot = fixture.surface.snapshot(1);
    REQUIRE(snapshot.lines.size() == 1);
    CHECK(snapshot.lines[0].size() == ColumnCount(10));
    // Clipped at the last column, not wrapped: the head of the text stays on the one row, and every
    // character past the edge overwrites the last column, as DECAWM reset does.
    CHECK(snapshot.lines[0].toUtf8() == "012345678N");
    CHECK(visibleText(snapshot.sgrText).size() == 10);
}

TEST_CASE("Surface.resize", "[surface]")
{
    auto fixture = SurfaceFixture { 10 };
    fixture.surface.resize(PageSize { .lines = LineCount(1), .columns = ColumnCount(30) });
    CHECK(fixture.surface.pageSize() == PageSize { .lines = LineCount(1), .columns = ColumnCount(30) });

    fixture.surface.feed("\033[14t");
    CHECK(fixture.surface.drainReplies() == "\033[4;16;240t");

    fixture.surface.feed(std::string(30, 'x'));
    auto snapshot = fixture.surface.snapshot(1);
    REQUIRE(snapshot.lines.size() == 1);
    CHECK(snapshot.lines[0].toUtf8() == std::string(30, 'x'));
}

TEST_CASE("Surface.snapshot.outlivesTheSurface", "[surface]")
{
    // A frame is handed to another thread and kept there: it must hold nothing of the surface's private
    // terminal, whose image pool's remover would otherwise be called from a dead terminal.
    auto fixture = std::make_unique<SurfaceFixture>(10);
    fixture->surface.feed("\033Pq#0;2;100;0;0#0!16~\033\\");
    auto snapshot =
        std::make_shared<vtbackend::StatusLineExtensionSnapshot const>(fixture->surface.snapshot(1));
    auto const& fragments = snapshot->lines.at(0).storage().imageFragments;
    REQUIRE(fragments.has_value());
    REQUIRE(fragments->size() == 2);
    // One image across both cells, as on the surface.
    CHECK(&fragments->at(0)->rasterizedImage() == &fragments->at(1)->rasterizedImage());

    // A host terminal takes the frame over and keeps showing it while the surface goes.
    struct TakeOver final: vtbackend::IStatusLineExtensions
    {
        std::shared_ptr<vtbackend::StatusLineExtensionSnapshot const> frame;
        [[nodiscard]] vtbackend::StatusLineEventKinds subscriptions() const noexcept override { return {}; }
        void post(vtbackend::StatusLineEvent /*event*/) noexcept override {}
        void resize(PageSize /*statusLineSize*/) noexcept override {}
        [[nodiscard]] std::shared_ptr<vtbackend::StatusLineExtensionSnapshot const> snapshot(
            std::string_view /*name*/) const noexcept override
        {
            return frame;
        }
        [[nodiscard]] std::optional<std::string> takeOverName() const noexcept override { return "plasma"; }
    };
    auto extensions = TakeOver {};
    extensions.frame = snapshot;
    auto sink = vtbackend::StatusLinePublishSink {};
    auto host = vtbackend::MockTerm { PageSize { .lines = LineCount(3), .columns = ColumnCount(10) },
                                      vtbackend::StatusLineExtensionServices {
                                          .extensions = extensions,
                                          .published = sink,
                                          .maxUserVars = vtextension::ExtensionLimits {}.maxUserVars } };
    host.terminal.setStatusDisplay(vtbackend::StatusDisplayType::Indicator);
    host.terminal.refreshRenderBuffer();
    REQUIRE(host.terminal.indicatorStatusLineDisplay()
                .at(vtbackend::LineOffset(0), ColumnOffset(0))
                .imageFragment()
            != nullptr);

    fixture.reset();
    CHECK(!fragments->at(0)->rasterizedImage().image().data().empty());
    extensions.frame.reset();
    snapshot.reset(); // the extension's last reference; the host still holds the frame it shows
    host.terminal.refreshRenderBuffer();
}
