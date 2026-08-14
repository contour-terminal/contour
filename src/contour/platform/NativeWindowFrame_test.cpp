// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/NativeWindowFrame.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>

using namespace contour::platform;

namespace
{
constexpr auto AllPlatforms =
    std::array { FramePlatform::Other, FramePlatform::Windows, FramePlatform::MacOS };
constexpr auto AllDecorations = std::array { WindowDecoration::Client, WindowDecoration::Server };
} // namespace

TEST_CASE("framePolicyFor keeps a native frame wherever one carries a shadow", "[contour][frame]")
{
    // The point of this whole file: it runs on every host, so the Windows and macOS decisions are
    // asserted on the Linux CI that can never execute their adapters.

    SECTION("Windows keeps its frame instead of going frameless")
    {
        // The regression. Qt::FramelessWindowHint is exactly what costs an HWND its DWM shadow, its
        // Win11 rounded corners and its Aero snap; keeping the frame and zeroing it in WM_NCCALCSIZE
        // gets all three back while the client area still covers the whole window.
        CHECK(framePolicyFor(FramePlatform::Windows, WindowDecoration::Client)
              == FramePolicy { .shadow = FrameShadowStrategy::NativeFrameKept,
                               .affordances = FrameAffordances::Client,
                               .controlPlacement = ControlPlacement::OwnBar,
                               .framelessHint = FramelessHint::Withhold });
    }

    SECTION("macOS keeps a decorated window and yields its controls in the same breath")
    {
        // These move TOGETHER or not at all: a full-size-content NSWindow still draws the real
        // traffic lights, so a policy that kept ours would show both sets -- and the content view
        // extends UNDER them, so the tab strip must also be told to leave their corner clear.
        auto const policy = framePolicyFor(FramePlatform::MacOS, WindowDecoration::Client);
        CHECK(policy.shadow == FrameShadowStrategy::FullSizeContent);
        CHECK(policy.affordances == FrameAffordances::Native);
        CHECK(policy.controlPlacement == ControlPlacement::OverChrome);
        CHECK(policy.framelessHint == FramelessHint::Withhold);
    }

    SECTION("everywhere else the window really is frameless")
    {
        // Which is what leaves the Linux/BSD shadow to the compositor, @see makeWindowShadow -- and
        // pins that none of the Windows or macOS work above changed the platform this ships on most.
        CHECK(framePolicyFor(FramePlatform::Other, WindowDecoration::Client)
              == FramePolicy { .shadow = FrameShadowStrategy::None,
                               .affordances = FrameAffordances::Client,
                               .controlPlacement = ControlPlacement::OwnBar,
                               .framelessHint = FramelessHint::Apply });
    }
}

TEST_CASE("framePolicyFor gives the whole frame to whoever draws it", "[contour][frame]")
{
    SECTION("a native title bar owns controls and resizing on every platform")
    {
        // One rule, not three: the gates in Main.qml read this rather than negating titleBarVisible,
        // so a platform added later cannot get a half-native frame by omission.
        for (auto const platform: AllPlatforms)
        {
            INFO("platform " << static_cast<int>(platform));
            auto const policy = framePolicyFor(platform, WindowDecoration::Server);
            CHECK(policy.affordances == FrameAffordances::Native);
            CHECK(policy.controlPlacement == ControlPlacement::OwnBar);
            CHECK(policy.framelessHint == FramelessHint::Withhold);
            CHECK(policy.shadow == FrameShadowStrategy::None);
        }
    }

    SECTION("a native shadow and the frameless hint never appear together")
    {
        // The invariant the whole table exists to hold. The hint removes the very frame the OS would
        // have shadowed, so any row asking for both is a contradiction the compiler cannot catch.
        for (auto const platform: AllPlatforms)
            for (auto const decoration: AllDecorations)
            {
                INFO("platform " << static_cast<int>(platform) << ", decoration "
                                 << static_cast<int>(decoration));
                auto const policy = framePolicyFor(platform, decoration);
                if (policy.shadow != FrameShadowStrategy::None)
                    CHECK(policy.framelessHint == FramelessHint::Withhold);
            }
    }

    SECTION("controls drawn over our chrome are always the OS's own")
    {
        // The converse cannot occur: only the OS can paint over its own content view, so a row
        // claiming OverChrome while WE draw the controls would be asking the tab strip to inset for
        // itself. This is what stops a platform added later inheriting the macOS answer by accident.
        for (auto const platform: AllPlatforms)
            for (auto const decoration: AllDecorations)
            {
                INFO("platform " << static_cast<int>(platform) << ", decoration "
                                 << static_cast<int>(decoration));
                auto const policy = framePolicyFor(platform, decoration);
                if (policy.controlPlacement == ControlPlacement::OverChrome)
                    CHECK(policy.affordances == FrameAffordances::Native);
            }
    }
}

TEST_CASE("ncCalcSizeInset only insets a maximized window", "[contour][frame]")
{
    constexpr auto frame = FrameInset { .left = 8, .top = 8, .right = 8, .bottom = 8 };

    SECTION("a normal window gets none, which is what makes the client area full-bleed")
    {
        CHECK(ncCalcSizeInset(WindowMaximized::No, frame) == FrameInset {});
        // Independent of the frame thickness, so it cannot go wrong on a DPI change.
        CHECK(ncCalcSizeInset(WindowMaximized::No, FrameInset { .left = 99 }) == FrameInset {});
    }

    SECTION("a maximized window is inset by the frame it would otherwise overhang by")
    {
        // Windows sizes a maximized window to the work area PLUS the frame; without this the
        // terminal's content spills onto the neighbouring monitor.
        CHECK(ncCalcSizeInset(WindowMaximized::Yes, frame) == frame);
    }
}

TEST_CASE("currentFramePlatform names the platform this binary was built for", "[contour][frame]")
{
#if defined(_WIN32)
    CHECK(currentFramePlatform() == FramePlatform::Windows);
#elif defined(__APPLE__)
    CHECK(currentFramePlatform() == FramePlatform::MacOS);
#else
    CHECK(currentFramePlatform() == FramePlatform::Other);
#endif
}
