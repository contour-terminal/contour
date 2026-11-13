// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/AppearanceServices.hpp>
#include <contour/platform/KWinTitleBandMaterial.hpp>
#include <contour/platform/TitleBandMaterial.hpp>

#include <QtGui/QWindow>

#include <catch2/catch_test_macros.hpp>

#include <expected>
#include <vector>

using contour::config::BandBackdrop;
using contour::platform::NativeTitleBarMetrics;
using contour::platform::NullTitleBandMaterial;
using contour::platform::TitleBandRequest;

TEST_CASE("the null material always succeeds, does nothing and measures nothing", "[titleband-material]")
{
    auto window = QWindow {};
    auto const flagsBefore = window.flags();
    auto material = NullTitleBandMaterial {};

    CHECK(material.apply(window, TitleBandRequest { .backdrop = BandBackdrop::NativeMaterial }).has_value());

    CHECK(window.flags() == flagsBefore);
    CHECK(material.metrics(window) == NativeTitleBarMetrics {});
}

TEST_CASE("the native factory is null off the Cocoa plugin", "[titleband-material]")
{
    // The suite runs under the offscreen QPA on every OS, so even the macOS material measures nothing.
    auto window = QWindow {};
    auto const material = contour::platform::makeTitleBandMaterial();
    REQUIRE(material != nullptr);
    CHECK(material->apply(window, TitleBandRequest { .backdrop = BandBackdrop::NativeMaterial }).has_value());
    CHECK(material->metrics(window) == NativeTitleBarMetrics {});
}

TEST_CASE("the null services build the null material", "[titleband-material]")
{
    auto const material = contour::platform::nullAppearanceServices().titleBand();
    CHECK(dynamic_cast<NullTitleBandMaterial const*>(material.get()) != nullptr);
}

TEST_CASE("a material error has a name for the log", "[titleband-material]")
{
    using contour::platform::TitleBandMaterialError;
    CHECK(contour::platform::titleBandMaterialErrorName(TitleBandMaterialError::Unsupported)
          == "unsupported");
    CHECK(contour::platform::titleBandMaterialErrorName(TitleBandMaterialError::Rejected) == "rejected");
}

namespace
{
/// One call the KWin material made to set or clear a band's blur.
struct BlurCall
{
    QWindow const* window = nullptr;
    qreal height = 0.0;
};

/// A KWin material over a recorder, on @p server and @p desktop, whose blur answers @p answer.
struct KWinFixture
{
    std::vector<BlurCall> calls {};
    std::expected<void, contour::platform::TitleBandMaterialError> answer {};
    contour::platform::KWinTitleBandMaterial material;

    KWinFixture(contour::config::HostPlatform desktop, contour::platform::DisplayServer server):
        material { desktop,
                   [server] { return server; },
                   [this](QWindow& window, qreal height) {
                       calls.push_back({ .window = &window, .height = height });
                       return answer;
                   } }
    {
    }
};
} // namespace

TEST_CASE("the KWin material blurs behind the band alone, and clears it for any other backdrop",
          "[titleband-material]")
{
    using contour::config::HostPlatform;
    using contour::platform::BandMaterialHost;
    using contour::platform::DisplayServer;

    auto window = QWindow {};
    auto fx = KWinFixture { HostPlatform::KdePlasma, DisplayServer::Wayland };
    // It needs no title bar of the OS's: a client-decorated window has none.
    CHECK(fx.material.host() == BandMaterialHost::Compositor);
    CHECK(fx.material.metrics(window) == NativeTitleBarMetrics {});

    // The band's height is the region blurred.
    CHECK(fx.material.apply(window, TitleBandRequest { .backdrop = BandBackdrop::Blurred, .height = 38.0 })
              .has_value());
    REQUIRE(fx.calls.size() == 1);
    CHECK(fx.calls.back().window == &window);
    CHECK(fx.calls.back().height == 38.0);

    // Any other backdrop clears it.
    CHECK(fx.material.apply(window, TitleBandRequest { .backdrop = BandBackdrop::Client, .height = 38.0 })
              .has_value());
    REQUIRE(fx.calls.size() == 2);
    CHECK(fx.calls.back().height == 0.0);
}

TEST_CASE("the KWin material is unsupported where nothing blurs behind the band", "[titleband-material]")
{
    using contour::config::HostPlatform;
    using contour::platform::DisplayServer;
    using contour::platform::TitleBandMaterialError;

    auto window = QWindow {};
    auto const blurred = TitleBandRequest { .backdrop = BandBackdrop::Blurred, .height = 38.0 };

    SECTION("a Wayland compositor without KWin's blur protocol says so itself")
    {
        auto fx = KWinFixture { HostPlatform::Other, DisplayServer::Wayland };
        fx.answer = std::unexpected(TitleBandMaterialError::Unsupported);
        auto const applied = fx.material.apply(window, blurred);
        REQUIRE_FALSE(applied.has_value());
        CHECK(applied.error() == TitleBandMaterialError::Unsupported);
        // Clearing such a blur fails too, and that is no reason for a solid band.
        CHECK(fx.material.apply(window, TitleBandRequest { .backdrop = BandBackdrop::Client }).has_value());
    }

    SECTION("an X11 window manager other than KWin is not asked: it would ignore the property")
    {
        auto fx = KWinFixture { HostPlatform::Other, DisplayServer::X11 };
        auto const applied = fx.material.apply(window, blurred);
        REQUIRE_FALSE(applied.has_value());
        CHECK(applied.error() == TitleBandMaterialError::Unsupported);
        CHECK(fx.calls.empty());
    }

    SECTION("KWin on X11 is trusted to blur")
    {
        auto fx = KWinFixture { HostPlatform::KdePlasma, DisplayServer::X11 };
        CHECK(fx.material.apply(window, blurred).has_value());
        CHECK(fx.calls.size() == 1);
    }

    SECTION("no windowing system with a compositor blur at all")
    {
        auto fx = KWinFixture { HostPlatform::KdePlasma, DisplayServer::Other };
        CHECK_FALSE(fx.material.apply(window, blurred).has_value());
        CHECK(fx.material.apply(window, TitleBandRequest { .backdrop = BandBackdrop::Client }).has_value());
        CHECK(fx.calls.empty());
    }
}
