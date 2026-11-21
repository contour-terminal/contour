// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/core/Image.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>

#include <vthost/ImageWire.hpp>

using namespace vthost;

TEST_CASE("image placement policies decode totally", "[vthost][imagewire]")
{
    // The codec carries these bytes unjudged, exactly as it does the mouse state, so the applier is
    // where a peer's byte becomes an enumerator — or does not. Casting straight into the enums put
    // values with no enumerator into `switch`es that end in std::unreachable() (Image.cpp's
    // computeTargetSize / computeTargetTopLeftOffset), reached once per frame on the render thread
    // for every attached pane: under -fsanitize=undefined the GUI aborts, and in a release build it
    // jumps into whatever follows the table, taking every other local tab in the process with it.

    SECTION("every in-range value keeps its meaning")
    {
        CHECK(imageLayerOf(0) == vtbackend::ImageLayer::Below);
        CHECK(imageLayerOf(2) == vtbackend::ImageLayer::Above);
        CHECK(imageResizeOf(0) == vtbackend::ImageResize::NoResize);
        CHECK(imageResizeOf(3) == vtbackend::ImageResize::StretchToFill);
        CHECK(imageAlignmentOf(0) == vtbackend::ImageAlignment::TopStart);
        CHECK(imageAlignmentOf(8) == vtbackend::ImageAlignment::BottomEnd);
        CHECK(imageFormatOf(2) == vtbackend::ImageFormat::RGBA);
        CHECK(imageOriginOf(0) == vtbackend::ImageOrigin::Cursor);
        CHECK(imageOriginOf(1) == vtbackend::ImageOrigin::Placeholder);
    }

    SECTION("an out-of-range value degrades to the policy's default")
    {
        CHECK(imageLayerOf(3) == vtbackend::ImageLayer::Replace);
        CHECK(imageLayerOf(255) == vtbackend::ImageLayer::Replace);
        CHECK(imageResizeOf(4) == vtbackend::ImageResize::ResizeToFit);
        CHECK(imageResizeOf(255) == vtbackend::ImageResize::ResizeToFit);
        CHECK(imageAlignmentOf(9) == vtbackend::ImageAlignment::MiddleCenter);
        CHECK(imageAlignmentOf(255) == vtbackend::ImageAlignment::MiddleCenter);
        // Auto is the one format an upload can never use: isConsistentPixmap rejects it outright,
        // so an unresolvable format cannot reach the GPU.
        CHECK(imageFormatOf(4) == vtbackend::ImageFormat::Auto);
        CHECK(imageFormatOf(255) == vtbackend::ImageFormat::Auto);
        // A cursor-placed image is the one that paints its own gap, so an unknown origin never hides it.
        CHECK(imageOriginOf(2) == vtbackend::ImageOrigin::Cursor);
        CHECK(imageOriginOf(255) == vtbackend::ImageOrigin::Cursor);
    }
}

TEST_CASE("image placement ordinals name a placement for as long as it lives", "[vthost][imagewire]")
{
    auto const makePlacement = [] {
        auto image = std::make_shared<vtbackend::Image>(
            vtbackend::ImageId(1),
            vtbackend::ImageFormat::RGBA,
            vtbackend::Image::Data(4, 0xFF),
            vtbackend::ImageSize { vtbackend::Width(1), vtbackend::Height(1) },
            [](auto) {});
        return vtbackend::rasterize(
            std::move(image),
            vtbackend::ImageAlignment::TopStart,
            vtbackend::ImageResize::NoResize,
            vtbackend::RGBAColor {},
            vtbackend::GridSize { .lines = vtbackend::LineCount(1), .columns = vtbackend::ColumnCount(1) },
            vtbackend::ImageSize { vtbackend::Width(1), vtbackend::Height(1) });
    };
    auto ordinals = ImagePlacementOrdinals {};
    auto first = makePlacement();
    auto const second = makePlacement();

    auto const firstOrdinal = ordinals.ordinalOf(*first);
    CHECK(ordinals.ordinalOf(*first) == firstOrdinal);
    CHECK(ordinals.ordinalOf(*second) != firstOrdinal);

    // A dead placement is forgotten, and whatever takes its address next is a new placement.
    first.reset();
    ordinals.prune();
    CHECK(ordinals.size() == 1);
    auto const third = makePlacement();
    auto const thirdOrdinal = ordinals.ordinalOf(*third);
    CHECK(thirdOrdinal != firstOrdinal);
    CHECK(thirdOrdinal != ordinals.ordinalOf(*second));
}
