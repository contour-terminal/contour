// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/AppearanceServices.hpp>
#include <contour/platform/TitleBandMaterial.hpp>

#include <QtGui/QWindow>

#include <catch2/catch_test_macros.hpp>

using contour::config::BandBackdrop;
using contour::platform::NativeTitleBarMetrics;
using contour::platform::NullTitleBandMaterial;
using contour::platform::TitleBandRequest;

TEST_CASE("the null material does nothing and measures nothing", "[titleband-material]")
{
    auto window = QWindow {};
    auto const flagsBefore = window.flags();
    auto material = NullTitleBandMaterial {};

    material.apply(window, TitleBandRequest { .backdrop = BandBackdrop::NativeMaterial });

    CHECK(window.flags() == flagsBefore);
    CHECK(material.metrics(window) == NativeTitleBarMetrics {});
}

TEST_CASE("the native factory is null off the Cocoa plugin", "[titleband-material]")
{
    // The suite runs under the offscreen QPA on every OS, so even the macOS material measures nothing.
    auto window = QWindow {};
    auto const material = contour::platform::makeTitleBandMaterial();
    REQUIRE(material != nullptr);
    material->apply(window, TitleBandRequest { .backdrop = BandBackdrop::NativeMaterial });
    CHECK(material->metrics(window) == NativeTitleBarMetrics {});
}

TEST_CASE("the null services build the null material", "[titleband-material]")
{
    auto const material = contour::platform::nullAppearanceServices().titleBand();
    CHECK(dynamic_cast<NullTitleBandMaterial const*>(material.get()) != nullptr);
}
