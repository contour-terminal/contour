// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/QuickTerminalPanel.hpp>

#ifdef __APPLE__

    #include <core/Assert.hpp>

    #include <QtGui/QGuiApplication>
    #include <QtGui/QWindow>

    #include <memory>

    #import <AppKit/AppKit.h>
    #import <Foundation/Foundation.h>

namespace contour::platform
{

namespace
{
    /// The NSPanel behind @p window, or nil. Qt's Cocoa plugin backs a Qt::Tool window with one.
    ///
    /// Deliberately not winId() on a window with no handle yet, which would create the platform window
    /// early, and nil under any platform plugin but cocoa, where winId() is not an NSView.
    NSPanel* panelOf(QWindow& window)
    {
        if (QGuiApplication::platformName() != QStringLiteral("cocoa") || window.handle() == nullptr)
            return nil;
        // Under the cocoa plugin a WId is the NSView*; ARC wants the bridge from an untyped pointer.
        auto* view = (__bridge NSView*) reinterpret_cast<void*>(window.winId());
        NSWindow* nsWindow = view != nil ? [view window] : nil;
        return [nsWindow isKindOfClass:[NSPanel class]] ? (NSPanel*) nsWindow : nil;
    }

    /// The quick terminal as a non-activating panel (the spike's candidate B, ruling S1): the window is
    /// created Qt::Tool | Qt::FramelessWindowHint, which Qt backs with an NSPanel; this adds
    /// NSWindowStyleMaskNonactivatingPanel so it can become key without making Contour the frontmost
    /// application, and presents it with makeKeyAndOrderFront: and no application activation, so the
    /// application the user came from stays frontmost and keeps its menu bar.
    class CocoaQuickTerminalPanel final: public IQuickTerminalPanel
    {
      public:
        /// @pre GUI (main) thread.
        void adopt(QWindow& window) override
        {
            Require(NSThread.isMainThread);
            // The platform window exists once the window has been created, which show() does; the style is
            // applied again at present(), because adopt() may come first.
            apply(window);
        }

        /// @pre GUI (main) thread.
        void present(QWindow& window, std::string const& /*activationToken*/) override
        {
            Require(NSThread.isMainThread);
            window.show(); // Creates the platform window on the first call.
            apply(window);
            if (NSPanel* panel = panelOf(window))
                [panel makeKeyAndOrderFront:nil];
            else
                window.requestActivate();
        }

        /// @pre GUI (main) thread.
        void dismiss(QWindow& window) override
        {
            Require(NSThread.isMainThread);
            if (NSPanel* panel = panelOf(window))
                [panel orderOut:nil];
            else
                window.hide();
        }

      private:
        /// Gives the panel its style and collection behaviour; does nothing until the platform window exists.
        static void apply(QWindow& window)
        {
            NSPanel* panel = panelOf(window);
            if (panel == nil)
                return;
            panel.styleMask |= NSWindowStyleMaskNonactivatingPanel;
            panel.floatingPanel = YES;
            panel.becomesKeyOnlyIfNeeded = NO;
            panel.hidesOnDeactivate = NO;
            panel.collectionBehavior |=
                NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorFullScreenAuxiliary;
        }
    };

} // namespace

std::unique_ptr<IQuickTerminalPanel> makeQuickTerminalPanel()
{
    return std::make_unique<CocoaQuickTerminalPanel>();
}

} // namespace contour::platform

#endif
