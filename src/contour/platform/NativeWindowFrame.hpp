// SPDX-License-Identifier: Apache-2.0
#pragma once

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
#if defined(_WIN32)
    return FramePlatform::Windows;
#elif defined(__APPLE__)
    return FramePlatform::MacOS;
#else
    return FramePlatform::Other;
#endif
}

/// Whether the window shows the operating system's own title bar.
///
/// What the QML-facing `titleBarVisible` bool converts to at the boundary, so nothing below this
/// line has to remember which way round the bool ran.
enum class TitleBarDecoration : uint8_t
{
    Client = 0, //!< Our tab strip is the whole decoration. The default.
    Native,     //!< The OS draws the title bar and its window controls.
};

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

/// Who draws the resize affordance. Gates ResizeBorder.qml.
enum class ResizeAffordance : uint8_t
{
    ClientDrawn = 0, //!< Our own hit zones, calling QWindow::startSystemResize.
    Native,          //!< The OS frame resizes the window itself.
};

/// Who draws the minimize/maximize/close controls. Gates the tab strip's own set.
enum class WindowControlsOwner : uint8_t
{
    Client = 0,
    Native,
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
    ResizeAffordance resize;
    WindowControlsOwner controls;
    FramelessHint framelessHint;

    [[nodiscard]] constexpr bool operator==(FramePolicy const&) const noexcept = default;
};

/// How @p platform should frame a window whose title bar is @p decoration.
///
/// A table, so supporting another platform is adding a row rather than editing four call sites.
/// Two invariants it encodes, both load-bearing:
///
///   - A native shadow and Qt::FramelessWindowHint are mutually exclusive. The hint is what removes
///     the frame the OS would have shadowed, so every row with a real @c shadow withholds it.
///   - Whoever draws the frame draws the controls and the resize affordance with it. Splitting
///     those would either duplicate the window controls or leave the window unresizable.
[[nodiscard]] constexpr FramePolicy framePolicyFor(FramePlatform platform,
                                                   TitleBarDecoration decoration) noexcept
{
    // The native title bar is one answer everywhere: the OS owns the frame, and everything with it.
    if (decoration == TitleBarDecoration::Native)
        return FramePolicy { .shadow = FrameShadowStrategy::None,
                             .resize = ResizeAffordance::Native,
                             .controls = WindowControlsOwner::Native,
                             .framelessHint = FramelessHint::Withhold };

    switch (platform)
    {
        case FramePlatform::Windows:
            // The frame is kept and then zeroed, so the client area still covers the whole window and
            // our tab strip still draws the controls -- but DWM sees a framed window and shadows it.
            return FramePolicy { .shadow = FrameShadowStrategy::NativeFrameKept,
                                 .resize = ResizeAffordance::ClientDrawn,
                                 .controls = WindowControlsOwner::Client,
                                 .framelessHint = FramelessHint::Withhold };

        case FramePlatform::MacOS:
            // The window stays a decorated NSWindow with a transparent, hidden title bar, so AppKit
            // keeps drawing the shadow, the rounded corners AND the traffic lights. Ours must
            // therefore switch off, or both sets appear.
            return FramePolicy { .shadow = FrameShadowStrategy::FullSizeContent,
                                 .resize = ResizeAffordance::Native,
                                 .controls = WindowControlsOwner::Native,
                                 .framelessHint = FramelessHint::Withhold };

        case FramePlatform::Other:
            // No OS frame service to keep, so the window really is frameless and its shadow is the
            // compositor's to grant. @see makeWindowShadow.
            return FramePolicy { .shadow = FrameShadowStrategy::None,
                                 .resize = ResizeAffordance::ClientDrawn,
                                 .controls = WindowControlsOwner::Client,
                                 .framelessHint = FramelessHint::Apply };
    }
    return FramePolicy { .shadow = FrameShadowStrategy::None,
                         .resize = ResizeAffordance::ClientDrawn,
                         .controls = WindowControlsOwner::Client,
                         .framelessHint = FramelessHint::Apply };
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
    virtual void apply(QWindow& window, TitleBarDecoration decoration) = 0;

    /// Re-asserts a shadow that a resize can leave stale.
    ///
    /// A no-op wherever the OS keeps its own shadow current; AppKit does not, because it derives the
    /// shadow from the window's alpha and recomputes it lazily.
    virtual void invalidateShadow(QWindow& window) = 0;
};

/// The frame that does nothing, for platforms with no OS frame service to ask.
class NullWindowFrame final: public NativeWindowFrame
{
  public:
    void apply(QWindow& /*window*/, TitleBarDecoration /*decoration*/) override {}
    void invalidateShadow(QWindow& /*window*/) override {}
};

/// The frame adapter for the platform this binary was built for.
///
/// @param colorScheme Supplies the effective dark/light scheme, which Win32 needs for the border DWM
///                    draws around the rounded corners. Injected rather than read from a global so
///                    the decision stays the configuration's (@see qtColorSchemeFor) and testable.
/// @return A NullWindowFrame where the platform offers nothing; never null.
[[nodiscard]] std::unique_ptr<NativeWindowFrame> makeNativeWindowFrame(
    std::function<Qt::ColorScheme()> colorScheme);

} // namespace contour::platform
