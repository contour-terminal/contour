// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/platform/WindowDecoration.hpp>

#include <QtCore/Qt>

#include <cstdint>
#include <functional>
#include <memory>

class QWindow;

namespace contour::platform
{

/// The host platform, as far as native-frame services for a client-decorated window go.
enum class FramePlatform : uint8_t
{
    Other = 0, //!< Everything with no such service: the Linux/BSD desktops.
    Windows,
    MacOS,
};

/// The platform this binary was built for.
[[nodiscard]] constexpr FramePlatform currentFramePlatform() noexcept
{
#ifdef _WIN32
    return FramePlatform::Windows;
#elifdef __APPLE__
    return FramePlatform::MacOS;
#else
    return FramePlatform::Other;
#endif
}

/// How a client-decorated window obtains a drop shadow from its operating system.
enum class FrameShadowStrategy : uint8_t
{
    /// None the OS can give; the compositor has to be asked separately, @see makeWindowShadow.
    None = 0,

    /// Win32: keep WS_THICKFRAME|WS_CAPTION and zero the frame in WM_NCCALCSIZE, so DWM still
    /// shadows and rounds a window whose client area covers all of it.
    NativeFrameKept,

    /// Cocoa: keep the NSWindow decorated but let the content fill its title bar.
    FullSizeContent,
};

/// Who supplies the window's frame affordances -- the resize handles and the window controls.
///
/// One field rather than two, because they are one decision: the frame that resizes the window is
/// the frame that carries its buttons, and splitting them would either duplicate the controls or
/// leave the window unresizable.
enum class FrameAffordances : uint8_t
{
    Client = 0, //!< Our own: ResizeBorder.qml's hit zones and the tab strip's own controls.
    Native,     //!< The OS frame's.
};

/// Where the OS puts its window controls, when it is the one drawing them.
enum class ControlPlacement : uint8_t
{
    OwnBar = 0, //!< In a title bar of its own, above our chrome. Costs us no space.
    OverChrome, //!< Over our content, which then has to leave their corner clear. Cocoa does this.
};

/// Whether Qt::FramelessWindowHint may be applied.
///
/// Named as an instruction rather than a question because withholding it is the ACTIVE choice on
/// two platforms: it is precisely the hint that costs a window its system shadow.
enum class FramelessHint : uint8_t
{
    Withhold = 0,
    Apply,
};

/// Everything that follows from a platform and a decoration choice.
struct FramePolicy
{
    FrameShadowStrategy shadow;
    FrameAffordances affordances;
    ControlPlacement controlPlacement;
    FramelessHint framelessHint;

    [[nodiscard]] constexpr bool operator==(FramePolicy const&) const noexcept = default;
};

namespace detail
{
    /// The OS owns the frame and everything on it. Every server-decorated row, on every platform.
    inline constexpr auto ServerFramePolicy = FramePolicy { .shadow = FrameShadowStrategy::None,
                                                            .affordances = FrameAffordances::Native,
                                                            .controlPlacement = ControlPlacement::OwnBar,
                                                            .framelessHint = FramelessHint::Withhold };

    /// A genuinely frameless window: we draw everything, and the shadow is the compositor's to grant.
    inline constexpr auto ClientFramePolicy = FramePolicy { .shadow = FrameShadowStrategy::None,
                                                            .affordances = FrameAffordances::Client,
                                                            .controlPlacement = ControlPlacement::OwnBar,
                                                            .framelessHint = FramelessHint::Apply };
} // namespace detail

/// How @p platform should frame a window whose title bar is @p decoration.
///
/// A table, so supporting another platform is adding a row rather than editing four call sites.
/// Three invariants it encodes, all load-bearing:
///
///   - A native shadow and Qt::FramelessWindowHint are mutually exclusive. The hint is what removes
///     the frame the OS would have shadowed, so every row with a real @c shadow withholds it.
///   - Whoever draws the frame draws the controls and the resize affordance with it, which is why
///     those are one @ref FrameAffordances field rather than two that must agree.
///   - Where the OS draws its controls OVER our chrome rather than in a bar of its own, our chrome
///     has to leave room; @c controlPlacement says which, so a platform added later cannot inherit
///     the answer by accident.
[[nodiscard]] constexpr FramePolicy framePolicyFor(FramePlatform platform,
                                                   WindowDecoration decoration) noexcept
{
    if (decoration == WindowDecoration::Server)
        return detail::ServerFramePolicy;

    switch (platform)
    {
        case FramePlatform::Windows:
            // The frame is kept and then zeroed, so the client area still covers the whole window and
            // our tab strip still draws the controls -- but DWM sees a framed window and shadows it.
            return FramePolicy { .shadow = FrameShadowStrategy::NativeFrameKept,
                                 .affordances = FrameAffordances::Client,
                                 .controlPlacement = ControlPlacement::OwnBar,
                                 .framelessHint = FramelessHint::Withhold };

        case FramePlatform::MacOS:
            // The window stays a decorated NSWindow with a transparent, hidden title bar, so AppKit
            // keeps drawing the shadow, the rounded corners AND the traffic lights -- over our own
            // chrome, since the content view extends under them. Ours must switch off and the tab
            // strip must leave their corner clear.
            return FramePolicy { .shadow = FrameShadowStrategy::FullSizeContent,
                                 .affordances = FrameAffordances::Native,
                                 .controlPlacement = ControlPlacement::OverChrome,
                                 .framelessHint = FramelessHint::Withhold };

        case FramePlatform::Other: return detail::ClientFramePolicy;
    }
    return detail::ClientFramePolicy;
}

/// A per-edge inset, in physical pixels.
struct FrameInset
{
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;

    [[nodiscard]] constexpr bool operator==(FrameInset const&) const noexcept = default;
};

/// Whether the window is currently maximized.
enum class WindowMaximized : uint8_t
{
    No = 0,
    Yes,
};

/// The inset WM_NCCALCSIZE applies to the client rectangle Windows proposes.
///
/// Zero on every edge for a normal window: that is what makes the client area full-bleed while DWM
/// still shadows, rounds and snaps it. A MAXIMIZED window is the exception -- Windows sizes it to
/// the work area PLUS the frame, so a zero inset there hangs the terminal's content over the
/// neighbouring monitor.
///
/// @param maximized Whether the window is maximized.
/// @param frame     The frame thickness for the window's CURRENT monitor DPI. Must come from
///                  AdjustWindowRectExForDpi rather than GetSystemMetrics, which answers for the
///                  primary monitor and is wrong on a mixed-DPI multi-monitor desktop.
[[nodiscard]] constexpr FrameInset ncCalcSizeInset(WindowMaximized maximized, FrameInset frame) noexcept
{
    return maximized == WindowMaximized::Yes ? frame : FrameInset {};
}

/// Applies the operating system's own frame decoration to a window.
///
/// An interface rather than a free function like @ref setBlurBehind: the Win32 implementation owns a
/// native event filter and a per-window state table for the lifetime of the application, and the
/// offscreen GUI tests substitute a recorder for it -- neither of which a free function with a
/// static map can offer.
class NativeWindowFrame
{
  public:
    NativeWindowFrame() = default;
    NativeWindowFrame(NativeWindowFrame const&) = delete;
    NativeWindowFrame& operator=(NativeWindowFrame const&) = delete;
    NativeWindowFrame(NativeWindowFrame&&) = delete;
    NativeWindowFrame& operator=(NativeWindowFrame&&) = delete;
    virtual ~NativeWindowFrame() = default;

    /// Applies @p decoration's policy to @p window.
    ///
    /// Called on every seed, not only on change, for the same reason the frameless hint is: a freshly
    /// adopted window has to be re-decorated even when the value already matched.
    virtual void apply(QWindow& window, WindowDecoration decoration) = 0;

    /// Re-asserts a shadow that a resize can leave stale.
    ///
    /// A no-op wherever the OS keeps its own shadow current; AppKit does not, because it derives the
    /// shadow from the window's alpha and recomputes it lazily.
    virtual void invalidateShadow(QWindow& window) = 0;

    /// Leading space @p window's chrome must leave clear for the OS's own window controls.
    ///
    /// Zero unless the policy says @ref ControlPlacement::OverChrome. Answered by the ADAPTER, in
    /// logical pixels, because the size is the operating system's: AppKit's traffic lights are a
    /// fixed size whatever font or style Contour is using, so computing it from our own chrome
    /// tokens gives a slot of the wrong width -- badly wrong under `ui_style: terminal`, where those
    /// tokens are counted in character cells.
    [[nodiscard]] virtual qreal nativeControlsInset(QWindow& window) const = 0;
};

/// The frame that does nothing, for platforms with no OS frame service to ask.
class NullWindowFrame final: public NativeWindowFrame
{
  public:
    void apply(QWindow& /*window*/, WindowDecoration /*decoration*/) override {}
    void invalidateShadow(QWindow& /*window*/) override {}
    [[nodiscard]] qreal nativeControlsInset(QWindow& /*window*/) const override { return 0.0; }
};

/// The frame adapter for the platform this binary was built for.
///
/// @param colorScheme Supplies the effective dark/light scheme, which Win32 needs for the border DWM
///                    draws around the rounded corners. Injected rather than read from a global so
///                    the decision stays the configuration's (@see qtColorSchemeFor) and testable.
/// @return A NullWindowFrame where the platform offers nothing; never null.
[[nodiscard]] std::unique_ptr<NativeWindowFrame> makeNativeWindowFrame(
    std::function<Qt::ColorScheme()> const& colorScheme);

} // namespace contour::platform
