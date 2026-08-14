// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/CocoaWindowFrame.hpp>

#ifdef __APPLE__

    #include <QtGui/QWindow>

    #include <map>

    #import <AppKit/AppKit.h>

namespace contour::platform
{

namespace
{
    /// The NSWindow behind @p window, or nil.
    ///
    /// Deliberately not winId(), which CREATES the platform window if there is none: the window is
    /// sized while still unmapped (WindowController::showInitial), and realizing it early is the
    /// trap WindowGeometry.hpp documents. A window with no handle yet is decorated on its first
    /// expose instead.
    NSWindow* nsWindowOf(QWindow& window)
    {
        if (window.handle() == nullptr)
            return nil;

        auto* view = reinterpret_cast<NSView*>(window.winId());
        return view != nil ? [view window] : nil;
    }
} // namespace

class CocoaWindowFrame final: public NativeWindowFrame
{
  public:
    void apply(QWindow& window, WindowDecoration decoration) override
    {
        NSWindow* nsWindow = nsWindowOf(window);
        if (nsWindow == nil)
            return;

        auto const policy = framePolicyFor(FramePlatform::MacOS, decoration);
        if (policy.shadow != FrameShadowStrategy::FullSizeContent)
        {
            // The native title bar: put the window back to an ordinary decorated one.
            nsWindow.styleMask &= ~NSWindowStyleMaskFullSizeContentView;
            nsWindow.titlebarAppearsTransparent = NO;
            nsWindow.titleVisibility = NSWindowTitleVisible;
            return;
        }

        // The content view covers the whole window, including where the title bar would be, but the
        // window stays DECORATED -- which is what keeps the shadow, the rounded corners and the
        // traffic lights. Our own tab strip then draws into that area, and reserves the corner the
        // traffic lights occupy (TitleBar.nativeControlsOverChrome).
        nsWindow.styleMask |= NSWindowStyleMaskFullSizeContentView;
        nsWindow.titlebarAppearsTransparent = YES;
        nsWindow.titleVisibility = NSWindowTitleHidden;

        // Belt and braces: a window whose content is fully transparent can end up with hasShadow
        // cleared, and without it none of the above shows.
        nsWindow.hasShadow = YES;
    }

    void invalidateShadow(QWindow& window) override
    {
        // AppKit derives the shadow from the window's alpha and recomputes it LAZILY, so a resize
        // leaves the previous outline behind until something asks. This is that ask; call it on
        // resize settle rather than per frame, since it recomputes an alpha mask.
        if (NSWindow* nsWindow = nsWindowOf(window))
            [nsWindow invalidateShadow];
    }

    /// The width of AppKit's own traffic-light cluster, measured rather than guessed.
    ///
    /// The buttons are a fixed size whatever font Contour is set in, so this is the one honest
    /// source for it -- and the reason the tab strip asks the adapter instead of composing it from
    /// chrome tokens, which under `ui_style: terminal` are counted in character cells.
    [[nodiscard]] qreal nativeControlsInset(QWindow& window) const override
    {
        NSWindow* nsWindow = nsWindowOf(window);
        if (nsWindow == nil)
            return 0.0;

        NSButton* close = [nsWindow standardWindowButton:NSWindowCloseButton];
        NSButton* zoom = [nsWindow standardWindowButton:NSWindowZoomButton];
        if (close == nil || zoom == nil)
            return 0.0;

        // Leading edge of the first button to the trailing edge of the last, plus the same gap
        // again so the first tab does not sit flush against the zoom button.
        auto const leading = NSMinX(close.frame);
        auto const trailing = NSMaxX(zoom.frame);
        return static_cast<qreal>(trailing + leading);
    }

  private:
    std::map<NSWindow*, WindowDecoration> _applied;
};

std::unique_ptr<NativeWindowFrame> makeCocoaWindowFrame()
{
    return std::make_unique<CocoaWindowFrame>();
}

} // namespace contour::platform

#endif
