// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/Image.hpp>

#include <catch2/catch_test_macros.hpp>

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
    }
}
