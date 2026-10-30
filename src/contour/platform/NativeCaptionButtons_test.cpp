// SPDX-License-Identifier: Apache-2.0
//
// The caption adapter's null object and its factory. The Win32 adapter, which answers WM_NCHITTEST, is
// tested on the native Windows platform only (Win32CaptionButtons_test.cpp).

#include <contour/platform/AppearanceServices.hpp>
#include <contour/platform/NativeCaptionButtons.hpp>
#include <contour/platform/QtPlatformPlugin.hpp>

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
#else
TEST_CASE("the factory builds the Win32 adapter, which leaves a non-native window alone", "[caption-buttons]")
{
    // Built before any window, as the application builds it before the QGuiApplication exists: the adapter
    // is the Win32 one on every Windows build, and decides on each call whether the plugin is native.
    auto const adapter = contour::platform::makeNativeCaptionButtons();
    REQUIRE(adapter != nullptr);
    CHECK(dynamic_cast<NullNativeCaptionButtons*>(adapter.get()) == nullptr);
    if (contour::platform::onNativeWindowsPlatform())
        SKIP("Covered under the native platform by Win32CaptionButtons_test.cpp.");

    auto calls = 0;
    auto window = QWindow {};
    window.create();
    adapter->attach(window,
                    CaptionBinding {
                        .layout =
                            [&calls] {
                                ++calls;
                                return CaptionLayout {};
                            },
                        .visual = [&calls](CaptionButtonVisual) { ++calls; },
                        .toggleMaximize = [&calls] { ++calls; },
                    });
    adapter->detach(window);
    CHECK(calls == 0);
}
#endif

TEST_CASE("the null services build the null adapter", "[caption-buttons]")
{
    auto const adapter = contour::platform::nullAppearanceServices().caption();
    REQUIRE(adapter != nullptr);
    CHECK(dynamic_cast<NullNativeCaptionButtons*>(adapter.get()) != nullptr);
}
