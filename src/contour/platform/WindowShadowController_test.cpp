// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/WindowShadowController.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace contour::platform;
using contour::config::ShadowSize;

namespace
{
/// Records what the controller asked the compositor to do, so a test can assert the SEQUENCE
/// rather than the outcome. None of this is observable through a real attachment.
class RecordingShadow final: public WindowShadow
{
  public:
    void apply(WindowShadowTiles const& tiles) override
    {
        calls.emplace_back("apply");
        lastOffsets = tiles.geometry.offsets;
        ++applyCount;
    }

    void withdraw() override { calls.emplace_back("withdraw"); }

    std::vector<std::string> calls;
    ShadowEdges lastOffsets {};
    int applyCount = 0;
};

/// The controller plus a borrowed view of its recorder, which the controller owns.
struct Harness
{
    RecordingShadow* recorder;
    WindowShadowController controller;

    static Harness make()
    {
        auto shadow = std::make_unique<RecordingShadow>();
        auto* borrowed = shadow.get();
        return Harness { .recorder = borrowed, .controller = WindowShadowController { std::move(shadow) } };
    }
};
} // namespace

TEST_CASE("WindowShadowController publishes only when a shadow is wanted", "[contour][shadow]")
{
    SECTION("a windowed, client-decorated window gets one")
    {
        auto harness = Harness::make();
        harness.controller.refresh(WindowPresentation::Windowed, WindowDecoration::Client, ShadowSize::Large);

        CHECK(harness.recorder->calls == std::vector<std::string> { "apply" });
        CHECK(harness.recorder->lastOffsets.bottom > harness.recorder->lastOffsets.top);
    }

    SECTION("a maximized window withdraws instead")
    {
        // Not merely "does not apply": a window that maximizes after being shown must actively
        // take its shadow back, or the compositor keeps reserving space beyond the work area.
        auto harness = Harness::make();
        harness.controller.refresh(WindowPresentation::Windowed, WindowDecoration::Client, ShadowSize::Large);
        harness.controller.refresh(
            WindowPresentation::Maximized, WindowDecoration::Client, ShadowSize::Large);

        CHECK(harness.recorder->calls == std::vector<std::string> { "apply", "withdraw" });
    }

    SECTION("restoring publishes again")
    {
        auto harness = Harness::make();
        harness.controller.refresh(WindowPresentation::Windowed, WindowDecoration::Client, ShadowSize::Large);
        harness.controller.refresh(
            WindowPresentation::FullScreen, WindowDecoration::Client, ShadowSize::Large);
        harness.controller.refresh(WindowPresentation::Windowed, WindowDecoration::Client, ShadowSize::Large);

        CHECK(harness.recorder->calls == std::vector<std::string> { "apply", "withdraw", "apply" });
    }

    SECTION("turning the native title bar on withdraws ours")
    {
        // The window manager draws its own shadow with its own decoration; a second one stacks.
        auto harness = Harness::make();
        harness.controller.refresh(WindowPresentation::Windowed, WindowDecoration::Client, ShadowSize::Large);
        harness.controller.refresh(WindowPresentation::Windowed, WindowDecoration::Server, ShadowSize::Large);

        CHECK(harness.recorder->calls == std::vector<std::string> { "apply", "withdraw" });
    }

    SECTION("configuring no shadow withdraws one already published")
    {
        auto harness = Harness::make();
        harness.controller.refresh(WindowPresentation::Windowed, WindowDecoration::Client, ShadowSize::Large);
        harness.controller.refresh(WindowPresentation::Windowed, WindowDecoration::Client, ShadowSize::None);

        CHECK(harness.recorder->calls == std::vector<std::string> { "apply", "withdraw" });
    }

    SECTION("a tiled window gets none")
    {
        auto harness = Harness::make();
        harness.controller.refresh(WindowPresentation::Tiled, WindowDecoration::Client, ShadowSize::Large);

        CHECK(harness.recorder->calls == std::vector<std::string> { "withdraw" });
    }
}

TEST_CASE("WindowShadowController re-renders only when the size changes", "[contour][shadow]")
{
    auto harness = Harness::make();

    SECTION("re-asserting the same state costs nothing but the apply")
    {
        // The window owner re-asserts on every state change it hears about, and several of those
        // arrive per resize; re-rendering the tiles each time would blur a megabyte per event.
        for (auto i = 0; i < 5; ++i)
            harness.controller.refresh(
                WindowPresentation::Windowed, WindowDecoration::Client, ShadowSize::Large);

        CHECK(harness.recorder->applyCount == 5);
        CHECK(harness.recorder->lastOffsets
              == shadowGeometryFor(shadowMetricsFor(ShadowSize::Large)).offsets);
    }

    SECTION("a different size produces a different shadow")
    {
        harness.controller.refresh(WindowPresentation::Windowed, WindowDecoration::Client, ShadowSize::Small);
        auto const small = harness.recorder->lastOffsets;

        harness.controller.refresh(
            WindowPresentation::Windowed, WindowDecoration::Client, ShadowSize::VeryLarge);

        CHECK(harness.recorder->lastOffsets.bottom > small.bottom);
    }
}

TEST_CASE("WindowShadowController withdraws on demand", "[contour][shadow]")
{
    // What the window's owner calls before closing, while the handle is still live.
    auto harness = Harness::make();
    harness.controller.refresh(WindowPresentation::Windowed, WindowDecoration::Client, ShadowSize::Large);
    harness.controller.withdraw();

    CHECK(harness.recorder->calls == std::vector<std::string> { "apply", "withdraw" });
}
