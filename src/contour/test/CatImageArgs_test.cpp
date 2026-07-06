// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the pure `contour cat` image-argument parsers extracted into CatImageArgs.h.

#include <contour/CatImageArgs.h>

#include <catch2/catch_test_macros.hpp>

using namespace contour;
using namespace std::string_view_literals;

TEST_CASE("parseCatSize accepts WxH and rejects malformed input", "[cat][image]")
{
    CHECK(parseCatSize("80x24"sv) == crispy::size { .width = 80, .height = 24 });
    CHECK(parseCatSize("1x1"sv) == crispy::size { .width = 1, .height = 1 });
    // No 'x', leading/trailing 'x', and non-numeric all yield {0,0}.
    CHECK(parseCatSize("80"sv) == crispy::size {});
    CHECK(parseCatSize("x24"sv) == crispy::size {});
    CHECK(parseCatSize("80x"sv) == crispy::size {});
    CHECK(parseCatSize("axb"sv) == crispy::size {});
    CHECK(parseCatSize(""sv) == crispy::size {});
}

TEST_CASE("parseCatAlignment maps every name and defaults to center", "[cat][image]")
{
    CHECK(parseCatAlignment("top-start"sv) == vtbackend::ImageAlignment::TopStart);
    CHECK(parseCatAlignment("top-center"sv) == vtbackend::ImageAlignment::TopCenter);
    CHECK(parseCatAlignment("top-end"sv) == vtbackend::ImageAlignment::TopEnd);
    CHECK(parseCatAlignment("middle-start"sv) == vtbackend::ImageAlignment::MiddleStart);
    CHECK(parseCatAlignment("middle-center"sv) == vtbackend::ImageAlignment::MiddleCenter);
    CHECK(parseCatAlignment("center"sv) == vtbackend::ImageAlignment::MiddleCenter);
    CHECK(parseCatAlignment("middle-end"sv) == vtbackend::ImageAlignment::MiddleEnd);
    CHECK(parseCatAlignment("bottom-start"sv) == vtbackend::ImageAlignment::BottomStart);
    CHECK(parseCatAlignment("bottom-center"sv) == vtbackend::ImageAlignment::BottomCenter);
    CHECK(parseCatAlignment("bottom-end"sv) == vtbackend::ImageAlignment::BottomEnd);
    // Unknown -> center.
    CHECK(parseCatAlignment("nonsense"sv) == vtbackend::ImageAlignment::MiddleCenter);
}

TEST_CASE("parseCatResize maps every policy and defaults to fit", "[cat][image]")
{
    CHECK(parseCatResize("no"sv) == vtbackend::ImageResize::NoResize);
    CHECK(parseCatResize("none"sv) == vtbackend::ImageResize::NoResize);
    CHECK(parseCatResize("fit"sv) == vtbackend::ImageResize::ResizeToFit);
    CHECK(parseCatResize("fill"sv) == vtbackend::ImageResize::ResizeToFill);
    CHECK(parseCatResize("stretch"sv) == vtbackend::ImageResize::StretchToFill);
    CHECK(parseCatResize("whatever"sv) == vtbackend::ImageResize::ResizeToFit);
}

TEST_CASE("parseCatLayer maps numeric and named layers and defaults to replace", "[cat][image]")
{
    CHECK(parseCatLayer("0"sv) == 0);
    CHECK(parseCatLayer("below"sv) == 0);
    CHECK(parseCatLayer("1"sv) == 1);
    CHECK(parseCatLayer("replace"sv) == 1);
    CHECK(parseCatLayer("2"sv) == 2);
    CHECK(parseCatLayer("above"sv) == 2);
    CHECK(parseCatLayer("xyz"sv) == 1);
}
