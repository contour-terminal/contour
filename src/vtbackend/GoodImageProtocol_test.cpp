// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/Image.h>
#include <vtbackend/MessageParser.h>
#include <vtbackend/MockTerm.h>
#include <vtbackend/Screen.h>
#include <vtbackend/test_helpers.h>

#include <crispy/base64.h>

#include <catch2/catch_test_macros.hpp>

#include <format>
#include <ranges>
#include <string>
#include <string_view>

using namespace vtbackend;
using namespace std::string_view_literals;

namespace
{

/// Helper: creates raw RGBA pixel data of the given size filled with the given color.
std::vector<uint8_t> makeRGBA(int width, int height, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    auto const pixelCount = static_cast<size_t>(width * height);
    auto data = std::vector<uint8_t>(pixelCount * 4);
    for (auto const i: std::views::iota(size_t { 0 }, pixelCount))
    {
        data[i * 4 + 0] = r;
        data[i * 4 + 1] = g;
        data[i * 4 + 2] = b;
        data[i * 4 + 3] = a;
    }
    return data;
}

/// Helper: wraps data as a GIP DCS upload sequence string.
/// DCS ! g o=u,<headers>;<body> ST
std::string gipUpload(std::string_view headers, std::span<uint8_t const> body)
{
    auto const encoded =
        crispy::base64::encode(std::string_view(reinterpret_cast<char const*>(body.data()), body.size()));
    return std::format("\033P!go=u,{};!{}\033\\", headers, encoded);
}

/// Helper: wraps a GIP DCS render sequence string.
std::string gipRender(std::string_view headers)
{
    return std::format("\033P!go=r,{}\033\\", headers);
}

/// Helper: wraps a GIP DCS oneshot sequence string.
std::string gipOneshot(std::string_view headers, std::span<uint8_t const> body)
{
    auto const encoded =
        crispy::base64::encode(std::string_view(reinterpret_cast<char const*>(body.data()), body.size()));
    return std::format("\033P!go=s,{};!{}\033\\", headers, encoded);
}

/// Helper: wraps a GIP DCS release sequence string.
std::string gipRelease(std::string_view headers)
{
    return std::format("\033P!go=d,{}\033\\", headers);
}

} // namespace

// ==================== Upload Tests ====================

TEST_CASE("GoodImageProtocol.Upload.RGB", "[GIP]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    auto const pixels = makeRGBA(2, 2, 0xFF, 0x00, 0x00, 0xFF);
    mock.writeToScreen(gipUpload("n=test,f=3,w=2,h=2", pixels));

    auto const imageRef = mock.terminal.imagePool().findImageByName("test");
    REQUIRE(imageRef != nullptr);
    CHECK(imageRef->width() == Width(2));
    CHECK(imageRef->height() == Height(2));
}

TEST_CASE("GoodImageProtocol.Upload.RGBA", "[GIP]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    auto const pixels = makeRGBA(3, 3, 0x00, 0xFF, 0x00, 0xFF);
    mock.writeToScreen(gipUpload("n=rgba,f=3,w=3,h=3", pixels));

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
    mock.writeToScreen(gipUpload("f=3,w=2,h=2", pixels));
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
    mock.writeToScreen(gipUpload("n=red,f=3,w=2,h=2", pixels));
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

    mock.writeToScreen(gipUpload("n=img,f=3,w=2,h=2", pixels));
    mock.resetReplyData();
    mock.writeToScreen(gipRender("n=img,c=4,r=2,s"));

    // CSI > 0 i = success
    CHECK(mock.replyData().find("\033P!gs=0\033\\") != std::string::npos);
}

TEST_CASE("GoodImageProtocol.Render.StatusFailure", "[GIP]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mock.resetReplyData();
    mock.writeToScreen(gipRender("n=missing,c=4,r=2,s"));

    // CSI > 1 i = failure
    CHECK(mock.replyData().find("\033P!gs=1\033\\") != std::string::npos);
}

// ==================== Oneshot Tests ====================

TEST_CASE("GoodImageProtocol.Oneshot.Render", "[GIP]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    auto const pixels = makeRGBA(2, 2, 0x00, 0x00, 0xFF, 0xFF);

    mock.writeToScreen(gipOneshot("f=3,w=2,h=2,c=4,r=2", pixels));

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

    mock.writeToScreen(gipUpload("n=tmp,f=3,w=2,h=2", pixels));
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

    // Response should contain ;90 (the GIP DA1 code)
    CHECK(mock.replyData().find(";90") != std::string::npos);
}

// ==================== Screen Layer Tests ====================

TEST_CASE("GoodImageProtocol.Layer.Below", "[GIP]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    auto const pixels = makeRGBA(2, 2, 0xFF, 0x00, 0x00, 0xFF);

    mock.writeToScreen(gipUpload("n=below,f=3,w=2,h=2", pixels));
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

    mock.writeToScreen(gipUpload("n=replace,f=3,w=2,h=2", pixels));
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

    mock.writeToScreen(gipUpload("n=above,f=3,w=2,h=2", pixels));
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
    mock.writeToScreen(gipUpload("n=zero,f=3,w=2,h=2", pixels));
    mock.writeToScreen(gipRender("n=zero,c=0,r=0"));

    // Cell should not have an image fragment.
    auto const& cell = mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0));
    CHECK(cell.imageFragment() == nullptr);
}

TEST_CASE("GoodImageProtocol.Oneshot.WithLayer", "[GIP]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    auto const pixels = makeRGBA(2, 2, 0x00, 0xFF, 0x00, 0xFF);

    mock.writeToScreen(gipOneshot("f=3,w=2,h=2,c=4,r=2,L=2", pixels));

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
    mock.writeToScreen(gipOneshot("f=3,w=2,h=2,c=4,r=2,L=0", pixels));

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
    mock.writeToScreen(gipOneshot("f=3,w=2,h=2,c=4,r=2", pixels));

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
    mock.writeToScreen(gipOneshot("f=3,w=2,h=2,c=4,r=2,L=2", pixels));

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
    mock.writeToScreen(gipOneshot("f=3,w=2,h=2,c=4,r=2,L=0", pixels));

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
    mock.writeToScreen(gipOneshot("f=3,w=2,h=2,c=4,r=2,L=0", pixels));

    // Verify fragment is placed.
    auto fragment = mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0)).imageFragment();
    REQUIRE(fragment != nullptr);

    // Erase display (ED 2 = clear entire screen). Reset operations should clear everything.
    mock.writeToScreen("\033[2J");

    // Below-layer image should be destroyed by erase.
    fragment = mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0)).imageFragment();
    CHECK(fragment == nullptr);
}

// ==================== Update-Cursor Tests ====================

TEST_CASE("GoodImageProtocol.Render.UpdateCursorFalse", "[GIP]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    auto const pixels = makeRGBA(2, 2, 0xFF, 0x00, 0x00, 0xFF);

    mock.writeToScreen(gipUpload("n=uc,f=3,w=2,h=2", pixels));

    // Move cursor to a known position.
    mock.writeToScreen("\033[3;5H"); // CUP to (3, 5) — 1-based
    auto const beforePos = mock.terminal.primaryScreen().realCursorPosition();

    // Render without u flag — cursor should NOT move.
    mock.writeToScreen(gipRender("n=uc,c=4,r=2"));

    auto const afterPos = mock.terminal.primaryScreen().realCursorPosition();
    CHECK(afterPos.line == beforePos.line);
    CHECK(afterPos.column == beforePos.column);
}

TEST_CASE("GoodImageProtocol.Render.UpdateCursorTrue", "[GIP]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    auto const pixels = makeRGBA(2, 2, 0xFF, 0x00, 0x00, 0xFF);

    mock.writeToScreen(gipUpload("n=uc2,f=3,w=2,h=2", pixels));

    // Move cursor to top-left.
    mock.writeToScreen("\033[1;1H"); // CUP to (1, 1) — 1-based
    // Render with u flag — cursor should move below the image.
    mock.writeToScreen(gipRender("n=uc2,c=4,r=3,u"));

    auto const afterPos = mock.terminal.primaryScreen().realCursorPosition();
    // Cursor should be at column 0 (left margin), line 3 (immediately below 3 rows of image, 0-based).
    CHECK(afterPos.column == ColumnOffset(0));
    CHECK(afterPos.line == LineOffset(3));
}

// ==================== Upload Status Response Tests ====================

TEST_CASE("GoodImageProtocol.Upload.StatusSuccess", "[GIP]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    auto const pixels = makeRGBA(2, 2, 0xFF, 0x00, 0x00, 0xFF);
    mock.resetReplyData();
    mock.writeToScreen(gipUpload("n=upstat,f=3,w=2,h=2,s", pixels));

    // CSI > 0 i = success
    CHECK(mock.replyData().find("\033P!gs=0\033\\") != std::string::npos);

    // Verify image was actually uploaded.
    auto const imageRef = mock.terminal.imagePool().findImageByName("upstat");
    CHECK(imageRef != nullptr);
}

TEST_CASE("GoodImageProtocol.Upload.StatusInvalidData", "[GIP]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    // Create data that doesn't match the declared dimensions (2x2 RGBA = 16 bytes, but we send 4).
    auto const wrongPixels = std::vector<uint8_t> { 0xFF, 0x00, 0x00, 0xFF };
    mock.resetReplyData();
    mock.writeToScreen(gipUpload("n=bad,f=3,w=2,h=2,s", wrongPixels));

    // CSI > 2 i = invalid image data
    CHECK(mock.replyData().find("\033P!gs=2\033\\") != std::string::npos);

    // Image should NOT be in pool.
    CHECK(mock.terminal.imagePool().findImageByName("bad") == nullptr);
}

// ==================== Name Validation Tests ====================

TEST_CASE("GoodImageProtocol.Upload.InvalidNameCharacters", "[GIP]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    auto const pixels = makeRGBA(1, 1, 0xFF, 0xFF, 0xFF, 0xFF);

    // Name with spaces should be rejected.
    mock.writeToScreen(gipUpload("n=bad name,f=3,w=1,h=1", pixels));
    CHECK(mock.terminal.imagePool().findImageByName("bad name") == nullptr);

    // Name with special characters should be rejected.
    mock.writeToScreen(gipUpload("n=bad!name,f=3,w=1,h=1", pixels));
    CHECK(mock.terminal.imagePool().findImageByName("bad!name") == nullptr);

    // Valid name with underscore should be accepted.
    mock.writeToScreen(gipUpload("n=good_name_123,f=3,w=1,h=1", pixels));
    CHECK(mock.terminal.imagePool().findImageByName("good_name_123") != nullptr);
}

// ==================== Query Tests ====================

TEST_CASE("GoodImageProtocol.Query.ResourceLimits", "[GIP]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mock.resetReplyData();

    // Send DCS ! g o=q ST
    mock.writeToScreen("\033P!go=q\033\\");

    // Response should be DCS ! g s=8,m=Pm,b=Pb,w=Pw,h=Ph ST
    auto const& reply = mock.replyData();
    CHECK(reply.find("\033P!gs=8,m=") != std::string::npos);
    CHECK(reply.find("\033\\") != std::string::npos);
}

// ==================== Oneshot Update-Cursor Tests ====================

TEST_CASE("GoodImageProtocol.Oneshot.UpdateCursorFalse", "[GIP]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    auto const pixels = makeRGBA(2, 2, 0xFF, 0x00, 0x00, 0xFF);

    // Move cursor to known position.
    mock.writeToScreen("\033[3;5H");
    auto const beforePos = mock.terminal.primaryScreen().realCursorPosition();

    // Oneshot without u flag — cursor should NOT move.
    mock.writeToScreen(gipOneshot("f=3,w=2,h=2,c=4,r=2", pixels));

    auto const afterPos = mock.terminal.primaryScreen().realCursorPosition();
    CHECK(afterPos.line == beforePos.line);
    CHECK(afterPos.column == beforePos.column);
}

TEST_CASE("GoodImageProtocol.Oneshot.UpdateCursorTrue", "[GIP]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    auto const pixels = makeRGBA(2, 2, 0xFF, 0x00, 0x00, 0xFF);

    // Move cursor to top-left.
    mock.writeToScreen("\033[1;1H");
    // Oneshot with u flag — cursor should move below the image.
    mock.writeToScreen(gipOneshot("f=3,w=2,h=2,c=4,r=2,u", pixels));

    auto const afterPos = mock.terminal.primaryScreen().realCursorPosition();
    // Cursor should be at column 0, line 2 (immediately below 2 rows, 0-based).
    CHECK(afterPos.column == ColumnOffset(0));
    CHECK(afterPos.line == LineOffset(2));
}

// ==================== Oneshot Status Response Tests ====================

TEST_CASE("GoodImageProtocol.Oneshot.StatusResponse", "[GIP]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    auto const pixels = makeRGBA(2, 2, 0xFF, 0x00, 0x00, 0xFF);
    mock.resetReplyData();

    mock.writeToScreen(gipOneshot("f=3,w=2,h=2,c=4,r=2,s", pixels));

    CHECK(mock.replyData().find("\033P!gs=0\033\\") != std::string::npos);
}

// ==================== Invalid Format Tests ====================

TEST_CASE("GoodImageProtocol.Oneshot.InvalidFormat", "[GIP]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    auto const pixels = makeRGBA(2, 2, 0xFF, 0x00, 0x00, 0xFF);

    // Format 9 is invalid — should not crash.
    mock.writeToScreen(gipOneshot("f=9,w=2,h=2,c=4,r=2", pixels));

    auto const& cell = mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0));
    (void) cell.imageFragment(); // no crash is the primary assertion
}

// ==================== Sub-Region Render Tests ====================

TEST_CASE("GoodImageProtocol.Render.SubRegion", "[GIP]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    // Set non-zero cell pixel size so that fragment() can compute target sizes without division by zero.
    mock.terminal.setCellPixelSize(ImageSize { Width(8), Height(16) });

    // Build a 4x2 RGBA image: left half is red (0xFF,0,0), right half is green (0,0xFF,0).
    auto pixels = std::vector<uint8_t>(static_cast<size_t>(4 * 2 * 4));
    for (auto const row: std::views::iota(0, 2))
    {
        for (auto const col: std::views::iota(0, 4))
        {
            auto const idx = static_cast<size_t>((row * 4 + col) * 4);
            auto const isRight = col >= 2;
            pixels[idx + 0] = isRight ? uint8_t { 0x00 } : uint8_t { 0xFF }; // R
            pixels[idx + 1] = isRight ? uint8_t { 0xFF } : uint8_t { 0x00 }; // G
            pixels[idx + 2] = 0x00;                                          // B
            pixels[idx + 3] = 0xFF;                                          // A
        }
    }

    mock.writeToScreen(gipUpload("n=sub,f=3,w=4,h=2", pixels));
    REQUIRE(mock.terminal.imagePool().findImageByName("sub") != nullptr);

    // Render only the right half: x=2, y=0, w=2, h=2 onto a 1x1 grid cell.
    mock.writeToScreen(gipRender("n=sub,c=1,r=1,x=2,y=0,w=2,h=2"));

    auto const& cell = mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0));
    auto const fragment = cell.imageFragment();
    REQUIRE(fragment != nullptr);

    // Extract the pixel data for this cell fragment.
    auto const cellPixelSize = mock.terminal.cellPixelSize();
    auto const fragmentData = fragment->rasterizedImage().fragment(
        CellLocation { .line = fragment->offset().line, .column = fragment->offset().column }, cellPixelSize);

    REQUIRE(fragmentData.size() == cellPixelSize.area() * 4);

    // Sample the center pixel of the fragment — it should be green (from the right half).
    auto const centerX = *cellPixelSize.width / 2;
    auto const centerY = *cellPixelSize.height / 2;
    auto const centerIdx = static_cast<size_t>((centerY * *cellPixelSize.width + centerX) * 4);
    CHECK(fragmentData[centerIdx + 0] == 0x00); // R = 0 (green, not red)
    CHECK(fragmentData[centerIdx + 1] == 0xFF); // G = 0xFF
    CHECK(fragmentData[centerIdx + 2] == 0x00); // B = 0
    CHECK(fragmentData[centerIdx + 3] == 0xFF); // A = 0xFF
}

// ==================== Auto Format Detection Tests ====================

/// Helper: creates raw RGB pixel data of the given size filled with the given color.
std::vector<uint8_t> makeRGB(int width, int height, uint8_t r, uint8_t g, uint8_t b)
{
    auto const pixelCount = static_cast<size_t>(width * height);
    auto data = std::vector<uint8_t>(pixelCount * 3);
    for (auto const i: std::views::iota(size_t { 0 }, pixelCount))
    {
        data[i * 3 + 0] = r;
        data[i * 3 + 1] = g;
        data[i * 3 + 2] = b;
    }
    return data;
}

TEST_CASE("GoodImageProtocol.Upload.AutoFormatRGBA", "[GIP]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    auto const pixels = makeRGBA(2, 2, 0xFF, 0x00, 0x00, 0xFF);

    // f=1 (Auto) with w=2, h=2 and 16 bytes of data -> detected as RGBA.
    mock.writeToScreen(gipUpload("n=auto_rgba,f=1,w=2,h=2", pixels));

    auto const imageRef = mock.terminal.imagePool().findImageByName("auto_rgba");
    REQUIRE(imageRef != nullptr);
    CHECK(imageRef->width() == Width(2));
    CHECK(imageRef->height() == Height(2));
}

TEST_CASE("GoodImageProtocol.Upload.AutoFormatRGB", "[GIP]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    auto const pixels = makeRGB(2, 2, 0xFF, 0x00, 0x00);

    // f=1 (Auto) with w=2, h=2 and 12 bytes of data -> detected as RGB.
    mock.writeToScreen(gipUpload("n=auto_rgb,f=1,w=2,h=2", pixels));

    auto const imageRef = mock.terminal.imagePool().findImageByName("auto_rgb");
    REQUIRE(imageRef != nullptr);
    CHECK(imageRef->width() == Width(2));
    CHECK(imageRef->height() == Height(2));
}

TEST_CASE("GoodImageProtocol.Upload.AutoFormatUnresolvable", "[GIP]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    // Random data that doesn't match PNG magic, and no w/h to detect RGB/RGBA.
    auto const randomData = std::vector<uint8_t> { 0x01, 0x02, 0x03, 0x04 };
    mock.resetReplyData();

    mock.writeToScreen(gipUpload("n=ambiguous,f=1,s", randomData));

    // Should fail with error code 2.
    CHECK(mock.replyData().find("\033P!gs=2\033\\") != std::string::npos);
    CHECK(mock.terminal.imagePool().findImageByName("ambiguous") == nullptr);
}

TEST_CASE("GoodImageProtocol.Oneshot.AutoFormatRGBA", "[GIP]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    auto const pixels = makeRGBA(2, 2, 0x00, 0x00, 0xFF, 0xFF);

    // f=1 (Auto) with w/h and RGBA-sized data.
    mock.writeToScreen(gipOneshot("f=1,w=2,h=2,c=4,r=2", pixels));

    auto const& cell = mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0));
    auto const fragment = cell.imageFragment();
    REQUIRE(fragment != nullptr);
}

TEST_CASE("GoodImageProtocol.Oneshot.DefaultFormatIsAuto", "[GIP]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    auto const pixels = makeRGBA(2, 2, 0x00, 0xFF, 0xFF, 0xFF);

    // No f= parameter at all; default should be Auto, which resolves to RGBA from data size.
    mock.writeToScreen(gipOneshot("w=2,h=2,c=4,r=2", pixels));

    auto const& cell = mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0));
    auto const fragment = cell.imageFragment();
    REQUIRE(fragment != nullptr);
}
