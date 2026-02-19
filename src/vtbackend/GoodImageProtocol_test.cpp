// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/Image.h>
#include <vtbackend/MessageParser.h>
#include <vtbackend/MockTerm.h>
#include <vtbackend/Screen.h>
#include <vtbackend/test_helpers.h>

#include <crispy/base64.h>

#include <catch2/catch_test_macros.hpp>

#include <format>
#include <string>
#include <string_view>

using namespace vtbackend;
using namespace std::string_view_literals;

namespace
{

/// Helper: creates raw RGBA pixel data of the given size filled with the given color.
std::vector<uint8_t> makeRGBA(int width, int height, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    auto data = std::vector<uint8_t>(static_cast<size_t>(width * height * 4));
    for (int i = 0; i < width * height; ++i)
    {
        data[static_cast<size_t>(i * 4 + 0)] = r;
        data[static_cast<size_t>(i * 4 + 1)] = g;
        data[static_cast<size_t>(i * 4 + 2)] = b;
        data[static_cast<size_t>(i * 4 + 3)] = a;
    }
    return data;
}

/// Helper: wraps data as a GIP DCS upload sequence string.
/// DCS u <headers>;<body> ST
std::string gipUpload(std::string_view headers, std::span<uint8_t const> body)
{
    auto const encoded =
        crispy::base64::encode(std::string_view(reinterpret_cast<char const*>(body.data()), body.size()));
    return std::format("\033Pu{};!{}\033\\", headers, encoded);
}

/// Helper: wraps a GIP DCS render sequence string.
std::string gipRender(std::string_view headers)
{
    return std::format("\033Pr{}\033\\", headers);
}

/// Helper: wraps a GIP DCS oneshot sequence string.
std::string gipOneshot(std::string_view headers, std::span<uint8_t const> body)
{
    auto const encoded =
        crispy::base64::encode(std::string_view(reinterpret_cast<char const*>(body.data()), body.size()));
    return std::format("\033Ps{};!{}\033\\", headers, encoded);
}

/// Helper: wraps a GIP DCS release sequence string.
std::string gipRelease(std::string_view headers)
{
    return std::format("\033Pd{}\033\\", headers);
}

} // namespace

// ==================== Upload Tests ====================

TEST_CASE("GoodImageProtocol.Upload.RGB", "[GIP]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    auto const pixels = makeRGBA(2, 2, 0xFF, 0x00, 0x00, 0xFF);
    mock.writeToScreen(gipUpload("n=test,f=2,w=2,h=2", pixels));

    auto const imageRef = mock.terminal.imagePool().findImageByName("test");
    REQUIRE(imageRef != nullptr);
    CHECK(imageRef->width() == Width(2));
    CHECK(imageRef->height() == Height(2));
}

TEST_CASE("GoodImageProtocol.Upload.RGBA", "[GIP]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    auto const pixels = makeRGBA(3, 3, 0x00, 0xFF, 0x00, 0xFF);
    mock.writeToScreen(gipUpload("n=rgba,f=2,w=3,h=3", pixels));

    auto const imageRef = mock.terminal.imagePool().findImageByName("rgba");
    REQUIRE(imageRef != nullptr);
    CHECK(imageRef->width() == Width(3));
    CHECK(imageRef->height() == Height(3));
}

TEST_CASE("GoodImageProtocol.Upload.WithoutName", "[GIP]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    auto const pixels = makeRGBA(2, 2, 0xFF, 0xFF, 0xFF, 0xFF);
    // Upload without name should be silently ignored.
    mock.writeToScreen(gipUpload("f=2,w=2,h=2", pixels));
    // No crash, no named image in pool.
}

TEST_CASE("GoodImageProtocol.Upload.InvalidFormat", "[GIP]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    auto const pixels = makeRGBA(1, 1, 0xFF, 0xFF, 0xFF, 0xFF);
    // Format 9 is invalid.
    mock.writeToScreen(gipUpload("n=invalid,f=9,w=1,h=1", pixels));

    auto const imageRef = mock.terminal.imagePool().findImageByName("invalid");
    CHECK(imageRef == nullptr);
}

// ==================== Render Tests ====================

TEST_CASE("GoodImageProtocol.Render.ByName", "[GIP]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    auto const pixels = makeRGBA(2, 2, 0xFF, 0x00, 0x00, 0xFF);

    // Upload first
    mock.writeToScreen(gipUpload("n=red,f=2,w=2,h=2", pixels));
    REQUIRE(mock.terminal.imagePool().findImageByName("red") != nullptr);

    // Render: 4 columns, 2 rows
    mock.writeToScreen(gipRender("n=red,c=4,r=2"));

    // Verify image fragments are placed in grid cells.
    auto const& cell = mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0));
    auto const fragment = cell.imageFragment();
    REQUIRE(fragment != nullptr);
    CHECK(fragment->rasterizedImage().image().width() == Width(2));
}

TEST_CASE("GoodImageProtocol.Render.NonexistentName", "[GIP]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };

    // Render a name that was never uploaded — should be a no-op, no crash.
    mock.writeToScreen(gipRender("n=nonexistent,c=4,r=2"));

    auto const& cell = mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0));
    auto const fragment = cell.imageFragment();
    CHECK(fragment == nullptr);
}

TEST_CASE("GoodImageProtocol.Render.StatusSuccess", "[GIP]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    auto const pixels = makeRGBA(2, 2, 0xFF, 0x00, 0x00, 0xFF);

    mock.writeToScreen(gipUpload("n=img,f=2,w=2,h=2", pixels));
    mock.resetReplyData();
    mock.writeToScreen(gipRender("n=img,c=4,r=2,s"));

    // CSI > 0 i = success
    CHECK(mock.replyData().find("\033[>0i") != std::string::npos);
}

TEST_CASE("GoodImageProtocol.Render.StatusFailure", "[GIP]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mock.resetReplyData();
    mock.writeToScreen(gipRender("n=missing,c=4,r=2,s"));

    // CSI > 1 i = failure
    CHECK(mock.replyData().find("\033[>1i") != std::string::npos);
}

// ==================== Oneshot Tests ====================

TEST_CASE("GoodImageProtocol.Oneshot.Render", "[GIP]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    auto const pixels = makeRGBA(2, 2, 0x00, 0x00, 0xFF, 0xFF);

    mock.writeToScreen(gipOneshot("f=2,w=2,h=2,c=4,r=2", pixels));

    // Verify image fragment in cell (0,0)
    auto const& cell = mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0));
    auto const fragment = cell.imageFragment();
    REQUIRE(fragment != nullptr);
}

// ==================== Release Tests ====================

TEST_CASE("GoodImageProtocol.Release.ByName", "[GIP]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    auto const pixels = makeRGBA(2, 2, 0xFF, 0xFF, 0xFF, 0xFF);

    mock.writeToScreen(gipUpload("n=tmp,f=2,w=2,h=2", pixels));
    REQUIRE(mock.terminal.imagePool().findImageByName("tmp") != nullptr);

    mock.writeToScreen(gipRelease("n=tmp"));
    CHECK(mock.terminal.imagePool().findImageByName("tmp") == nullptr);
}

TEST_CASE("GoodImageProtocol.Release.Nonexistent", "[GIP]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    // Releasing a nonexistent name should be a no-op, no crash.
    mock.writeToScreen(gipRelease("n=nope"));
}

// ==================== DA1 Test ====================

TEST_CASE("GoodImageProtocol.DA1.IncludesGIPCode", "[GIP]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mock.resetReplyData();
    // Send DA1 query
    mock.writeToScreen("\033[c");
    mock.terminal.flushInput();

    // Response should contain ;11 (the GIP DA1 code)
    CHECK(mock.replyData().find(";11") != std::string::npos);
}

// ==================== Screen Layer Tests ====================

TEST_CASE("GoodImageProtocol.Layer.Below", "[GIP]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    auto const pixels = makeRGBA(2, 2, 0xFF, 0x00, 0x00, 0xFF);

    mock.writeToScreen(gipUpload("n=below,f=2,w=2,h=2", pixels));
    mock.writeToScreen(gipRender("n=below,c=4,r=2,L=0"));

    auto const& cell = mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0));
    auto const fragment = cell.imageFragment();
    REQUIRE(fragment != nullptr);
    CHECK(fragment->rasterizedImage().layer() == ImageLayer::Below);
}

TEST_CASE("GoodImageProtocol.Layer.Replace", "[GIP]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    auto const pixels = makeRGBA(2, 2, 0xFF, 0x00, 0x00, 0xFF);

    mock.writeToScreen(gipUpload("n=replace,f=2,w=2,h=2", pixels));
    // Default layer (no L parameter) should be Replace.
    mock.writeToScreen(gipRender("n=replace,c=4,r=2"));

    auto const& cell = mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0));
    auto const fragment = cell.imageFragment();
    REQUIRE(fragment != nullptr);
    CHECK(fragment->rasterizedImage().layer() == ImageLayer::Replace);
}

TEST_CASE("GoodImageProtocol.Layer.Above", "[GIP]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    auto const pixels = makeRGBA(2, 2, 0xFF, 0x00, 0x00, 0xFF);

    mock.writeToScreen(gipUpload("n=above,f=2,w=2,h=2", pixels));
    mock.writeToScreen(gipRender("n=above,c=4,r=2,L=2"));

    auto const& cell = mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0));
    auto const fragment = cell.imageFragment();
    REQUIRE(fragment != nullptr);
    CHECK(fragment->rasterizedImage().layer() == ImageLayer::Above);
}

// ==================== Edge Cases ====================

TEST_CASE("GoodImageProtocol.EdgeCase.ZeroGridSize", "[GIP]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    auto const pixels = makeRGBA(2, 2, 0xFF, 0xFF, 0xFF, 0xFF);

    // Render with r=0, c=0 should not crash.
    mock.writeToScreen(gipUpload("n=zero,f=2,w=2,h=2", pixels));
    mock.writeToScreen(gipRender("n=zero,c=0,r=0"));

    // Cell should not have an image fragment.
    auto const& cell = mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0));
    CHECK(cell.imageFragment() == nullptr);
}

TEST_CASE("GoodImageProtocol.Oneshot.WithLayer", "[GIP]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    auto const pixels = makeRGBA(2, 2, 0x00, 0xFF, 0x00, 0xFF);

    mock.writeToScreen(gipOneshot("f=2,w=2,h=2,c=4,r=2,L=2", pixels));

    auto const& cell = mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0));
    auto const fragment = cell.imageFragment();
    REQUIRE(fragment != nullptr);
    CHECK(fragment->rasterizedImage().layer() == ImageLayer::Above);
}

// ==================== MessageParser MaxBodyLength ====================

TEST_CASE("GoodImageProtocol.MaxBodyLength", "[GIP]")
{
    CHECK(MessageParser::MaxBodyLength == 16 * 1024 * 1024);
}

// ==================== Layer Text-Write Interaction Tests ====================

TEST_CASE("GoodImageProtocol.Layer.Below.SurvivesTextWrite", "[GIP]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    auto const pixels = makeRGBA(2, 2, 0xFF, 0x00, 0x00, 0xFF);

    // Place a Below-layer image at cursor position (top-left).
    mock.writeToScreen(gipOneshot("f=2,w=2,h=2,c=4,r=2,L=0", pixels));

    // Verify fragment is placed.
    auto fragment = mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0)).imageFragment();
    REQUIRE(fragment != nullptr);
    CHECK(fragment->rasterizedImage().layer() == ImageLayer::Below);

    // Move cursor back to top-left and write text over the image area.
    mock.writeToScreen("\033[H"); // CUP to (1,1)
    mock.writeToScreen("ABCD");

    // Below-layer image should survive the text write.
    fragment = mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0)).imageFragment();
    CHECK(fragment != nullptr);

    // Text should also be present.
    auto const& cell = mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0));
    CHECK(cell.codepoint(0) == U'A');
}

TEST_CASE("GoodImageProtocol.Layer.Replace.DestroyedByTextWrite", "[GIP]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    auto const pixels = makeRGBA(2, 2, 0xFF, 0x00, 0x00, 0xFF);

    // Place a Replace-layer image (default layer).
    mock.writeToScreen(gipOneshot("f=2,w=2,h=2,c=4,r=2", pixels));

    // Verify fragment is placed with Replace layer.
    auto fragment = mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0)).imageFragment();
    REQUIRE(fragment != nullptr);
    CHECK(fragment->rasterizedImage().layer() == ImageLayer::Replace);

    // Move cursor back and write text.
    mock.writeToScreen("\033[H");
    mock.writeToScreen("ABCD");

    // Replace-layer image should be destroyed by text write.
    fragment = mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0)).imageFragment();
    CHECK(fragment == nullptr);

    // Text should be present.
    auto const& cell = mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0));
    CHECK(cell.codepoint(0) == U'A');
}

TEST_CASE("GoodImageProtocol.Layer.Above.SurvivesTextWrite", "[GIP]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    auto const pixels = makeRGBA(2, 2, 0xFF, 0x00, 0x00, 0xFF);

    // Place an Above-layer image.
    mock.writeToScreen(gipOneshot("f=2,w=2,h=2,c=4,r=2,L=2", pixels));

    // Verify fragment is placed.
    auto fragment = mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0)).imageFragment();
    REQUIRE(fragment != nullptr);
    CHECK(fragment->rasterizedImage().layer() == ImageLayer::Above);

    // Move cursor back and write text.
    mock.writeToScreen("\033[H");
    mock.writeToScreen("ABCD");

    // Above-layer image should survive the text write.
    fragment = mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0)).imageFragment();
    CHECK(fragment != nullptr);

    // Text should also be present.
    auto const& cell = mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0));
    CHECK(cell.codepoint(0) == U'A');
}

TEST_CASE("GoodImageProtocol.Layer.Below.SurvivesCursorMoveAndTextWrite", "[GIP]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    auto const pixels = makeRGBA(2, 2, 0xFF, 0x00, 0x00, 0xFF);

    // Place a Below-layer image spanning 4 columns x 2 rows at top-left.
    mock.writeToScreen(gipOneshot("f=2,w=2,h=2,c=4,r=2,L=0", pixels));

    // Write some text elsewhere (after the image area).
    mock.writeToScreen("extra text");

    // Move cursor back to top-left and overwrite all 4 image columns.
    mock.writeToScreen("\033[H"); // CUP to (1,1)
    mock.writeToScreen("WXYZ");

    // All 4 cells on the first row should retain their image fragments.
    for (auto col: { 0, 1, 2, 3 })
    {
        auto const fragment =
            mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(col)).imageFragment();
        CHECK(fragment != nullptr);
    }

    // Text should also be present in those cells.
    CHECK(mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0)).codepoint(0) == U'W');
    CHECK(mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(1)).codepoint(0) == U'X');
    CHECK(mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(2)).codepoint(0) == U'Y');
    CHECK(mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(3)).codepoint(0) == U'Z');
}

TEST_CASE("GoodImageProtocol.Layer.Below.ClearedByErase", "[GIP]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    auto const pixels = makeRGBA(2, 2, 0xFF, 0x00, 0x00, 0xFF);

    // Place a Below-layer image.
    mock.writeToScreen(gipOneshot("f=2,w=2,h=2,c=4,r=2,L=0", pixels));

    // Verify fragment is placed.
    auto fragment = mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0)).imageFragment();
    REQUIRE(fragment != nullptr);

    // Erase display (ED 2 = clear entire screen). Reset operations should clear everything.
    mock.writeToScreen("\033[2J");

    // Below-layer image should be destroyed by erase.
    fragment = mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0)).imageFragment();
    CHECK(fragment == nullptr);
}
