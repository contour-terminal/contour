// SPDX-License-Identifier: Apache-2.0
//
// The caption adapter's null object and its factory. The Win32 adapter, which answers WM_NCHITTEST, is
// tested on the native Windows platform only (Win32CaptionButtons_test.cpp).

#include <contour/platform/AppearanceServices.hpp>
#include <contour/platform/NativeCaptionButtons.hpp>

#include <QtGui/QWindow>

#include <catch2/catch_test_macros.hpp>

using contour::platform::CaptionBinding;
using contour::platform::CaptionButtonVisual;
using contour::platform::CaptionLayout;
using contour::platform::NullNativeCaptionButtons;

TEST_CASE("the null adapter ignores attach and detach", "[caption-buttons]")
{
    auto calls = 0;
    auto window = QWindow {};
    auto adapter = NullNativeCaptionButtons {};

    adapter.attach(window,
                   CaptionBinding {
                       .layout =
                           [&calls] {
                               ++calls;
                               return CaptionLayout {};
                           },
                       .visual = [&calls](CaptionButtonVisual) { ++calls; },
                       .toggleMaximize = [&calls] { ++calls; },
                   });
    adapter.detach(window);
    // Detaching a window never attached is ignored too.
    auto other = QWindow {};
    adapter.detach(other);

    CHECK(calls == 0);
}

#ifndef _WIN32
TEST_CASE("the factory is null off Windows", "[caption-buttons]")
{
    auto const adapter = contour::platform::makeNativeCaptionButtons();
    REQUIRE(adapter != nullptr);
    CHECK(dynamic_cast<NullNativeCaptionButtons*>(adapter.get()) != nullptr);
}
#endif

TEST_CASE("the null services build the null adapter", "[caption-buttons]")
{
    auto const adapter = contour::platform::nullAppearanceServices().caption();
    REQUIRE(adapter != nullptr);
    CHECK(dynamic_cast<NullNativeCaptionButtons*>(adapter.get()) != nullptr);
}
