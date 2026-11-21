// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/state/ImageState.hpp>
#include <vtbackend/state/StateFields.hpp>
#include <vtbackend/testing/MockTerm.hpp>

#include <core/Base64.hpp>

#include <libunicode/convert.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>

using namespace vtbackend;
using namespace std::string_view_literals;

namespace
{
using Mock = MockTerm<vtpty::MockPty>;

constexpr auto CellPixels = ImageSize { Width(2), Height(2) };

/// A 2x2 RGBA image, base64-encoded as the kitty protocol carries it.
std::string encodedPixels()
{
    auto pixels = std::string {};
    for ([[maybe_unused]] auto const pixel: std::views::iota(0, 4))
        pixels += "\xFF\x00\x00\xFF"sv;
    return core::base64::encode(pixels);
}

std::unique_ptr<Mock> makeTerminal()
{
    auto mock = std::make_unique<Mock>(PageSize { LineCount(4), ColumnCount(8) }, LineCount(4));
    mock->terminal.setCellPixelSize(CellPixels);
    return mock;
}

/// Everything about images that a capture of the primary screen holds.
struct Captured
{
    ImageTableState table;
    GridState grid;
    KittyScreenState kitty;
};

Captured capture(Mock& mock)
{
    auto& screen = mock.terminal.primaryScreen();
    auto builder = ImageTableBuilder { mock.terminal.imagePool() };
    auto result = Captured { .table = {},
                             .grid = screen.grid().captureState(builder),
                             .kitty = screen.captureKittyState(builder) };
    result.table = std::move(builder).finish();
    return result;
}

/// @return @p value through the byte codec.
template <typename T>
T throughCodec(T const& value)
{
    auto writer = StateWriter::payload();
    detail::encodeFields(writer, value);
    auto const bytes = std::move(writer).take();
    auto decoded = T {};
    REQUIRE(detail::decodeFields(bytes, decoded).has_value());
    return decoded;
}

/// Sends a capture through the byte codec and back.
Captured throughBytes(Captured const& captured)
{
    return Captured { .table = throughCodec(captured.table),
                      .grid = throughCodec(captured.grid),
                      .kitty = throughCodec(captured.kitty) };
}

/// Restores @p captured into @p target and returns the resolver that keeps the images alive.
RestoredImageTable restoreInto(Mock& target, Captured const& captured)
{
    auto restored = RestoredImageTable::build(target.terminal.imagePool(), captured.table);
    REQUIRE(restored.has_value());
    restored->commit(target.terminal.imagePool());
    auto& screen = target.terminal.primaryScreen();
    REQUIRE(screen.grid().restoreState(captured.grid, *restored).has_value());
    REQUIRE(screen.restoreKittyState(captured.kitty, *restored).has_value());
    return *std::move(restored);
}
} // namespace

TEST_CASE("one image referenced by fragments, kitty table and an animation frame is one object after restore",
          "[ImageState]")
{
    auto source = makeTerminal();
    source->writeToScreen(std::format("\033_Ga=T,f=32,s=2,v=2,i=31;{}\033\\", encodedPixels()));
    source->writeToScreen(std::format("\033_Ga=f,i=31,r=2,s=2,v=2;{}\033\\", encodedPixels()));
    auto const& sourceScreen = source->terminal.primaryScreen();
    auto const sourceImage = sourceScreen.kittyImage(31);
    REQUIRE(sourceImage);
    REQUIRE(sourceScreen.at(LineOffset(0), ColumnOffset(0)).imageFragment());
    REQUIRE(&sourceScreen.at(LineOffset(0), ColumnOffset(0)).imageFragment()->rasterizedImage().image()
            == sourceImage.get());
    REQUIRE(sourceScreen.kittyFrameImage(31, 1) == sourceImage);
    REQUIRE(sourceScreen.kittyFrameImage(31, 2));

    auto const captured = throughBytes(capture(*source));
    CHECK(captured.table.images.size() == 2); // The transmitted image, and the second frame's.
    CHECK(captured.table.rasterized.size() == 1);

    auto target = makeTerminal();
    auto const keepAlive = restoreInto(*target, captured);

    auto const& screen = target->terminal.primaryScreen();
    auto const restoredImage = screen.kittyImage(31);
    REQUIRE(restoredImage);
    auto const fragment = screen.at(LineOffset(0), ColumnOffset(0)).imageFragment();
    REQUIRE(fragment);
    CHECK(&fragment->rasterizedImage().image() == restoredImage.get());
    CHECK(screen.kittyFrameImage(31, 1).get() == restoredImage.get());
    CHECK(screen.kittyFrameImage(31, 2).get() != restoredImage.get());
    CHECK(restoredImage->data() == sourceImage->data());
    CHECK(restoredImage->id() == sourceImage->id());
}

TEST_CASE("fragments of one placement share a single RasterizedImage after restore", "[ImageState]")
{
    auto source = makeTerminal();
    // 4x2 pixels at 2x2 cells: two cells wide.
    auto pixels = std::string {};
    for ([[maybe_unused]] auto const pixel: std::views::iota(0, 8))
        pixels += "\x00\xFF\x00\xFF"sv;
    source->writeToScreen(std::format("\033_Ga=T,f=32,s=4,v=2,i=5;{}\033\\", core::base64::encode(pixels)));
    auto const& sourceScreen = source->terminal.primaryScreen();
    auto const first = sourceScreen.at(LineOffset(0), ColumnOffset(0)).imageFragment();
    auto const second = sourceScreen.at(LineOffset(0), ColumnOffset(1)).imageFragment();
    REQUIRE((first && second));
    REQUIRE(&first->rasterizedImage() == &second->rasterizedImage());

    auto target = makeTerminal();
    auto const keepAlive = restoreInto(*target, throughBytes(capture(*source)));

    auto const& screen = target->terminal.primaryScreen();
    auto const restoredFirst = screen.at(LineOffset(0), ColumnOffset(0)).imageFragment();
    auto const restoredSecond = screen.at(LineOffset(0), ColumnOffset(1)).imageFragment();
    REQUIRE((restoredFirst && restoredSecond));
    CHECK(&restoredFirst->rasterizedImage() == &restoredSecond->rasterizedImage());
    CHECK(restoredFirst->offset() == first->offset());
    CHECK(restoredSecond->offset() == second->offset());
}

TEST_CASE("a kitty placement keeps its placement id across restore", "[ImageState]")
{
    auto source = makeTerminal();
    source->writeToScreen(std::format("\033_Ga=T,f=32,s=2,v=2,i=5,p=7;{}\033\\", encodedPixels()));
    auto const placed = source->terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0)).imageFragment();
    REQUIRE(placed);
    REQUIRE(placed->rasterizedImage().kittyPlacementId() == KittyPlacementId(7));

    auto target = makeTerminal();
    auto const keepAlive = restoreInto(*target, throughBytes(capture(*source)));

    auto const restored = target->terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0)).imageFragment();
    REQUIRE(restored);
    CHECK(restored->rasterizedImage().kittyPlacementId() == KittyPlacementId(7));

    // Deleting by that placement id still finds the restored placement.
    target->writeToScreen("\033_Ga=d,d=i,i=5,p=7;\033\\"sv);
    CHECK_FALSE(target->terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0)).imageFragment());
}

TEST_CASE("restored images keep their ids and the pool continues numbering after them", "[ImageState]")
{
    auto source = makeTerminal();
    for (auto const id: std::views::iota(10, 13))
        source->writeToScreen(std::format("\033_Ga=t,f=32,s=2,v=2,i={};{}\033\\", id, encodedPixels()));
    auto const linked =
        source->terminal.imagePool().create(ImageFormat::RGBA, CellPixels, Image::Data(16, 7));
    source->terminal.imagePool().link("logo", linked);
    auto const nextInSource = source->terminal.imagePool().nextImageId();

    auto target = makeTerminal();
    auto const keepAlive = restoreInto(*target, throughBytes(capture(*source)));

    for (auto const id: std::views::iota(10, 13))
    {
        auto const original = source->terminal.primaryScreen().kittyImage(static_cast<uint32_t>(id));
        auto const restored = target->terminal.primaryScreen().kittyImage(static_cast<uint32_t>(id));
        REQUIRE((original && restored));
        CHECK(restored->id() == original->id());
        CHECK(target->terminal.imagePool().findImageById(original->id()) == restored);
    }
    auto const byName = target->terminal.imagePool().findImageByName("logo");
    REQUIRE(byName);
    CHECK(byName->id() == linked->id());
    CHECK(byName->data() == linked->data());

    CHECK(target->terminal.imagePool().nextImageId() == nextInSource);
    auto const fresh = target->terminal.imagePool().create(ImageFormat::RGBA, CellPixels, Image::Data(16, 0));
    CHECK(fresh->id() == nextInSource);
}

TEST_CASE("kitty image placed by id after restore resolves", "[ImageState]")
{
    auto source = makeTerminal();
    source->writeToScreen(std::format("\033_Ga=t,f=32,s=2,v=2,i=31;{}\033\\", encodedPixels()));
    auto target = makeTerminal();
    auto const keepAlive = restoreInto(*target, throughBytes(capture(*source)));

    source->terminal.flushInput();
    target->terminal.flushInput();
    source->writeToScreen("\033_Gi=31,a=p;\033\\"sv);
    target->writeToScreen("\033_Gi=31,a=p;\033\\"sv);

    CHECK_FALSE(target->terminal.peekInput().contains("ENOENT"));
    auto const expected = source->terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0)).imageFragment();
    auto const actual = target->terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0)).imageFragment();
    REQUIRE(expected);
    REQUIRE(actual);
    CHECK(actual->offset() == expected->offset());
    CHECK(actual->rasterizedImage().image().id() == expected->rasterizedImage().image().id());
    CHECK(actual->rasterizedImage().cellSpan().columns == expected->rasterizedImage().cellSpan().columns);
}

TEST_CASE("an interrupted chunked kitty transfer completes after restore", "[ImageState]")
{
    auto const encoded = encodedPixels();
    REQUIRE(encoded.size() % 8 == 0);
    auto const half = encoded.size() / 2;

    auto source = makeTerminal();
    source->writeToScreen(std::format("\033_Ga=t,f=32,s=2,v=2,i=40,m=1;{}\033\\", encoded.substr(0, half)));
    REQUIRE_FALSE(source->terminal.primaryScreen().kittyImage(40));

    auto const captured = throughBytes(capture(*source));
    REQUIRE(captured.kitty.chunked.has_value());
    CHECK(captured.kitty.chunked->payload == encoded.substr(0, half));
    CHECK(captured.kitty.chunked->command.imageId == 40);

    auto target = makeTerminal();
    auto const keepAlive = restoreInto(*target, captured);
    source->writeToScreen(std::format("\033_Gm=0;{}\033\\", encoded.substr(half)));
    target->writeToScreen(std::format("\033_Gm=0;{}\033\\", encoded.substr(half)));

    auto const expected = source->terminal.primaryScreen().kittyImage(40);
    auto const actual = target->terminal.primaryScreen().kittyImage(40);
    REQUIRE((expected && actual));
    CHECK(actual->data() == expected->data());
    CHECK(actual->size().width == expected->size().width);
}

TEST_CASE("an image table survives the byte codec unchanged", "[ImageState]")
{
    auto source = makeTerminal();
    source->writeToScreen(std::format("\033_Ga=T,f=32,s=2,v=2,i=1;{}\033\\", encodedPixels()));
    source->terminal.imagePool().link(
        "a", source->terminal.imagePool().create(ImageFormat::RGBA, CellPixels, Image::Data(16, 1)));
    auto const captured = capture(*source);
    auto const decoded = throughBytes(captured);
    CHECK(decoded.table == captured.table);
    CHECK(decoded.kitty == captured.kitty);
    CHECK(decoded.grid == captured.grid);
}

TEST_CASE("an inconsistent image table is refused and leaves the pool untouched", "[ImageState]")
{
    auto pool = ImagePool {};
    auto table = ImageTableState {};
    table.images.push_back({ .id = ImageId(3),
                             .format = ImageFormat::RGBA,
                             .data = std::vector<uint8_t>(16),
                             .size = CellPixels });
    table.nextImageId = ImageId(4);

    SECTION("a duplicate id")
    {
        table.images.push_back(table.images.front());
    }
    SECTION("pixels that do not fill the declared size")
    {
        table.images.front().data.resize(15);
    }
    SECTION("a rasterized image of an unknown image")
    {
        table.rasterized.push_back({ .image = ImageId(99) });
    }
    SECTION("a name for an unknown image")
    {
        table.names.push_back({ .name = "x", .image = ImageId(99) });
    }
    SECTION("a fragment of an unknown rasterized image")
    {
        table.fragments.push_back({ .rasterized = 5, .offset = {} });
    }

    auto const restored = RestoredImageTable::build(pool, table);
    REQUIRE_FALSE(restored.has_value());
    CHECK(restored.error() == StateDecodeError::InvalidValue);
    CHECK(pool.nextImageId() == ImageId(1));
    CHECK_FALSE(pool.findImageById(ImageId(3)));
}

TEST_CASE("a kitty table naming an image the resolver lacks is refused", "[ImageState]")
{
    auto mock = makeTerminal();
    auto state = KittyScreenState {};
    state.images.emplace_back(7, ImageId(123));
    auto const before = mock->terminal.primaryScreen().captureKittyState(
        *std::make_unique<ImageTableBuilder>(mock->terminal.imagePool()));
    auto const result = mock->terminal.primaryScreen().restoreKittyState(state, NoImagesResolver {});
    CHECK(result == std::unexpected { StateDecodeError::InvalidValue });
    CHECK(mock->terminal.primaryScreen().captureKittyState(
              *std::make_unique<ImageTableBuilder>(mock->terminal.imagePool()))
          == before);
}

TEST_CASE("kitty command fields all survive the codec", "[ImageState]")
{
    auto state = KittyScreenState {};
    auto command = kitty_graphics::Command {};
    command.action = kitty_graphics::Action::Frame;
    command.format = kitty_graphics::Format::Png;
    command.medium = kitty_graphics::Medium::SharedMemory;
    command.compression = kitty_graphics::Compression::ZlibDeflate;
    command.imageId = 1;
    command.imageNumber = 2;
    command.placementId = 3;
    command.pixelWidth = 4;
    command.pixelHeight = 5;
    command.sourceX = 6;
    command.sourceY = 7;
    command.sourceWidth = 8;
    command.sourceHeight = 9;
    command.columns = 10;
    command.rows = 11;
    command.zIndex = -12;
    command.doNotMoveCursor = true;
    command.moreChunksFollow = true;
    command.quietAlways = true;
    command.deleteTarget = 'I';
    command.baseFrame = 13;
    command.currentFrame = 14;
    command.targetFrame = 15;
    command.frameGapMilliseconds = -16;
    command.animationState = kitty_graphics::AnimationState::Loop;
    command.loopCount = 17;
    command.frameBackground = 0xAABBCCDD;
    command.compositionMode = kitty_graphics::CompositionMode::Replace;
    command.rawX = 18;
    command.composeSourceFrame = 19;
    command.composeDestinationFrame = 20;
    command.composeSourceX = 21;
    command.composeSourceY = 22;
    command.composeDestinationX = 23;
    command.composeDestinationY = 24;
    command.placementKind = kitty_graphics::PlacementKind::Virtual;
    command.queryFeature = 'T';
    command.payload = "payload";
    state.chunked = KittyChunkState { .payload = "so far", .command = command };

    auto const decoded = std::optional { throughCodec(state) };
    REQUIRE(decoded->chunked.has_value());
    CHECK(decoded->chunked->payload == "so far");
    auto const& copy = decoded->chunked->command;
    CHECK(copy.action == command.action);
    CHECK(copy.format == command.format);
    CHECK(copy.medium == command.medium);
    CHECK(copy.zIndex == -12);
    CHECK(copy.frameGapMilliseconds == -16);
    CHECK(copy.doNotMoveCursor);
    CHECK(copy.moreChunksFollow);
    CHECK_FALSE(copy.quietOnSuccess);
    CHECK(copy.quietAlways);
    CHECK(copy.deleteTarget == 'I');
    CHECK(copy.frameBackground == 0xAABBCCDD);
    CHECK(copy.payload == "payload");
    CHECK(copy.loopCount == 17);
    CHECK(copy.animationState == command.animationState);
    CHECK(copy.placementKind == kitty_graphics::PlacementKind::Virtual);
    CHECK(copy.queryFeature == 'T');
    CHECK(copy == command);
}

namespace
{
/// @return Image row @p row of a placeholder picture of image @p id (by palette index), @p columns cells
/// wide,
///         as Claude Code prints it.
std::string placeholderRow(int id, size_t row, size_t columns)
{
    constexpr auto Diacritics = std::array { U'̅', U'̍', U'̎', U'̐' };
    auto text = std::u32string {};
    for (auto const column: std::views::iota(size_t { 0 }, columns))
        text += std::u32string { U'\U0010EEEE', Diacritics.at(row), Diacritics.at(column) };
    return std::format(
        "\033[38;5;{}m{}\033[m\r\n", id, unicode::convert_to<char>(std::u32string_view { text }));
}
} // namespace

TEST_CASE("kitty virtual placements and image numbers survive a restore", "[ImageState][state]")
{
    // A live upgrade in the middle of a Claude Code session: a resolved placeholder picture, a placeholder
    // line whose image has not arrived yet, and an image stored under a number.
    auto source = makeTerminal();
    source->writeToScreen(
        std::format("\033_Ga=T,U=1,i=7,c=4,r=2,f=32,s=2,v=2,q=2;{}\033\\", encodedPixels()));
    source->writeToScreen(std::format("\033_Ga=t,I=5,f=32,s=2,v=2,q=2;{}\033\\", encodedPixels()));
    source->writeToScreen(placeholderRow(7, 0, 4) + placeholderRow(7, 1, 4) + placeholderRow(8, 0, 4));
    REQUIRE(source->terminal.primaryScreen().at(LineOffset(1), ColumnOffset(3)).imageFragment());
    REQUIRE_FALSE(source->terminal.primaryScreen().at(LineOffset(2), ColumnOffset(0)).imageFragment());

    auto const captured = throughBytes(capture(*source));
    auto target = makeTerminal();
    auto const keepAlive = restoreInto(*target, captured);
    auto& screen = target->terminal.primaryScreen();

    SECTION("the virtual placement is back, and the picture's cells show its very raster")
    {
        auto const* placement = screen.findKittyVirtualPlacement(7, KittyPlacementId {});
        REQUIRE(placement != nullptr);
        CHECK(placement->raster->origin() == ImageOrigin::Placeholder);
        for (auto const row: std::views::iota(0, 2))
            for (auto const column: std::views::iota(0, 4))
            {
                INFO(std::format("cell ({}, {})", row, column));
                auto const fragment = screen.at(LineOffset(row), ColumnOffset(column)).imageFragment();
                REQUIRE(fragment);
                CHECK(&fragment->rasterizedImage() == placement->raster.get());
                CHECK(fragment->offset()
                      == CellLocation { .line = LineOffset(row), .column = ColumnOffset(column) });
            }
    }

    SECTION("the unresolved line resolves once its image arrives")
    {
        target->writeToScreen(
            std::format("\033_Ga=T,U=1,i=8,c=4,r=1,f=32,s=2,v=2,q=2;{}\033\\", encodedPixels()));
        auto const fragment = screen.at(LineOffset(2), ColumnOffset(3)).imageFragment();
        REQUIRE(fragment);
        CHECK(fragment->rasterizedImage().origin() == ImageOrigin::Placeholder);
    }

    SECTION("the image number still names its image")
    {
        // This partial restore brings back no cursor, so the place is chosen explicitly.
        target->writeToScreen("\033[4;1H\033_Ga=p,I=5,c=1,r=1,C=1,q=2\033\\");
        CHECK(screen.at(LineOffset(3), ColumnOffset(0)).imageFragment());
    }
}
