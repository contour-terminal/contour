// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/CocoaWindowFrame.hpp>

#ifdef __APPLE__

    #include <QtGui/QWindow>

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
    void apply(QWindow& window, TitleBarDecoration decoration) override
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
};

std::unique_ptr<NativeWindowFrame> makeCocoaWindowFrame()
{
    return std::make_unique<CocoaWindowFrame>();
}

} // namespace contour::platform

#endif
