// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/TitleBandMaterial.hpp>

#ifdef __APPLE__

    #include <core/Assert.hpp>

    #include <QtCore/QMargins>
    #include <QtGui/QGuiApplication>
    #include <QtGui/QWindow>

    #include <algorithm>
    #include <memory>

    #import <AppKit/AppKit.h>

    // The title bar's height is Qt's safe-area top under Qt::ExpandedClientAreaHint; configure rejects an
    // older Qt on macOS (src/contour/CMakeLists.txt), and this says why if that is bypassed.
    #if QT_VERSION < QT_VERSION_CHECK(6, 9, 0)
        #error "macOS builds need Qt 6.9 or later (Qt::ExpandedClientAreaHint, QWindow::safeAreaMargins)."
    #endif

namespace contour::platform
{

namespace
{
    /// The one toolbar this material attaches, known again by its identifier.
    NSString* const titleBandToolbarIdentifier = @"org.contourterminal.titleband";

    /// @return Whether the Cocoa platform plugin is running. Asked at call time, not at construction: the
    ///         material is built before a QGuiApplication exists (TitleBandMaterial.hpp).
    [[nodiscard]] bool onCocoa()
    {
        return QGuiApplication::platformName() == QStringLiteral("cocoa");
    }

    /// The NSWindow behind @p window, or nil. Never winId() on a window with no handle: that would create the
    /// platform window, which the pre-show sizing must not see (CocoaWindowFrame.mm says why).
    [[nodiscard]] NSWindow* nsWindowOf(QWindow& window)
    {
        if (window.handle() == nullptr || !onCocoa())
            return nil;
        // Under the cocoa plugin a WId is the NSView*; ARC wants the bridge from an untyped pointer.
        auto* const handle = reinterpret_cast<void*>(window.winId());
        auto* const view = (__bridge NSView*) handle;
        return view != nil ? view.window : nil;
    }

    /// @return The appearance that draws @p scheme; nil follows the application's.
    [[nodiscard]] NSAppearance* appearanceFor(Qt::ColorScheme scheme)
    {
        switch (scheme)
        {
            case Qt::ColorScheme::Dark: return [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
            case Qt::ColorScheme::Light: return [NSAppearance appearanceNamed:NSAppearanceNameAqua];
            case Qt::ColorScheme::Unknown: break;
        }
        return nil;
    }

    /// @return Whether @p window wears the toolbar this material attached.
    [[nodiscard]] bool wearsOurToolbar(NSWindow* window)
    {
        return window.toolbar != nil &&
               [window.toolbar.identifier isEqualToString:titleBandToolbarIdentifier];
    }

    /// Grows @p window's title bar as @p growth asks: an empty compact toolbar, attached once, or none.
    void applyGrowth(NSWindow* window, config::TitleBarGrowth growth)
    {
        switch (growth)
        {
            case config::TitleBarGrowth::CompactToolbar:
                if (window.toolbar == nil)
                {
                    auto* const toolbar = [[NSToolbar alloc] initWithIdentifier:titleBandToolbarIdentifier];
                    toolbar.allowsUserCustomization = NO;
                    window.toolbar = toolbar;
                }
                if (wearsOurToolbar(window))
                    window.toolbarStyle = NSWindowToolbarStyleUnifiedCompact;
                return;
            case config::TitleBarGrowth::None:
                // Only a toolbar of ours: one somebody else attached is not this material's to take.
                if (wearsOurToolbar(window))
                    window.toolbar = nil;
                return;
        }
    }

    /// @return The window-relative top of a revealed full-screen title bar: below the menu bar or the notch,
    ///         less however far the window itself already starts below the screen's top; 0 when windowed.
    [[nodiscard]] qreal fullScreenTopOf(NSWindow* window)
    {
        if ((window.styleMask & NSWindowStyleMaskFullScreen) == 0 || window.screen == nil)
            return 0.0;
        // spike-decisions.md, ruling S5: the revealed bar sits max(menu bar, notch) below the screen's top.
        auto const screenTop = std::max(NSApp.mainMenu.menuBarHeight, window.screen.safeAreaInsets.top);
        auto const windowOffset = NSMaxY(window.screen.frame) - NSMaxY(window.frame);
        return std::max(0.0, static_cast<qreal>(screenTop - windowOffset));
    }

    /// AppKit's title bar around the macOS title band -- its appearance, its toolbar, its shadow -- and the
    /// measure of it.
    ///
    /// The shipped band asks for the client backdrop (ruling S6: under AppKit's own material, the
    /// NativeMaterial backdrop, which stays implemented but unselected, the NSTitlebarView takes the band's
    /// clicks). Per the Step 1d spike (spike-decisions.md): Qt's ExpandedClientAreaHint owns the full-size
    /// content view and reports the title bar as the safe area (D1 = Hint), so apply() leaves the title bar's
    /// transparency to Qt's NoTitleBarBackgroundHint, which the controller sets for the client backdrop; an
    /// empty compact toolbar grows the title bar to 40 pt around centred traffic lights (D2 =
    /// CompactToolbar); and a full-screen title bar is revealed max(menuBarHeight, safeAreaInsets.top) below
    /// the screen's top (S5). The shadow is asserted again after every apply(), as §3.5 asks: AppKit derives
    /// it lazily from the window's alpha.
    class CocoaTitleBandMaterial final: public ITitleBandMaterial
    {
      public:
        /// @pre GUI (main) thread.
        void apply(QWindow& window, TitleBandRequest const& request) override
        {
            Require(NSThread.isMainThread);
            NSWindow* const nsWindow = nsWindowOf(window);
            if (nsWindow == nil)
                return;

            // The backdrop is Qt's to draw (Qt::NoTitleBarBackgroundHint, set by the controller): nothing
            // here.
            nsWindow.appearance = appearanceFor(request.scheme);
            applyGrowth(nsWindow, request.growth);
            nsWindow.hasShadow = YES;
            [nsWindow invalidateShadow];
        }

        /// @pre GUI (main) thread.
        [[nodiscard]] NativeTitleBarMetrics metrics(QWindow& window) const override
        {
            Require(NSThread.isMainThread);
            NSWindow* const nsWindow = nsWindowOf(window);
            if (nsWindow == nil)
                return {};
            return NativeTitleBarMetrics { .height = static_cast<qreal>(window.safeAreaMargins().top()),
                                           .fullScreenTop = fullScreenTopOf(nsWindow) };
        }
    };

} // namespace

std::unique_ptr<ITitleBandMaterial> makeTitleBandMaterial()
{
    // Unconditionally the Cocoa one: this runs before a QGuiApplication exists, so the platform plugin is
    // asked when the material is called (F1), as CocoaNativeMenuPresenter asks it.
    return std::make_unique<CocoaTitleBandMaterial>();
}

} // namespace contour::platform

#endif
