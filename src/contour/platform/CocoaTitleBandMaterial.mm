// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/TitleBandMaterial.hpp>

#ifdef __APPLE__

    #include <core/Assert.hpp>

    #include <QtCore/QMargins>
    #include <QtGui/QGuiApplication>
    #include <QtGui/QWindow>

    #include <algorithm>
    #include <cstdint>
    #include <expected>
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

    /// The identifiers of the two views this material puts behind the Qt content: the frosted band's backdrop
    /// (BandBackdrop::Blurred) and the container of the glass tabs (TabSurface::Glass).
    NSString* const frostedBackdropIdentifier = @"org.contourterminal.titleband.backdrop.frosted";
    NSString* const glassBackdropIdentifier = @"org.contourterminal.titleband.glasstabs";

    /// @return The subview of @p frame carrying @p identifier, or nil.
    [[nodiscard]] NSView* subviewIdentified(NSView* frame, NSString* identifier)
    {
        for (NSView* view in frame.subviews)
            if ([view.identifier isEqualToString:identifier])
                return view;
        return nil;
    }

    /// @return The rectangle in @p frame's coordinates of a strip @p height tall across its top.
    [[nodiscard]] NSRect topStripOf(NSView* frame, CGFloat height)
    {
        auto const bounds = frame.bounds;
        return frame.isFlipped ? NSMakeRect(0, 0, NSWidth(bounds), height)
                               : NSMakeRect(0, NSHeight(bounds) - height, NSWidth(bounds), height);
    }

    /// Puts the frosted band's view behind @p window's Qt content, or takes it away.
    ///
    /// The title bar's own material (NSVisualEffectMaterialTitlebar, blended behind the window) with nothing
    /// painted over it: Apple asks apps to let the system draw the backgrounds of bars rather than lay their
    /// own over them. Behind the content view rather than in the title bar: a view of AppKit's title bar takes
    /// the band's clicks (F13), whereas one below the content view never sees them.
    void applyBackdropView(NSWindow* window, TitleBandRequest const& request)
    {
        NSView* const content = window.contentView;
        NSView* const frame = content.superview;
        if (frame == nil)
            return;

        NSView* view = subviewIdentified(frame, frostedBackdropIdentifier);
        if (request.backdrop != config::BandBackdrop::Blurred || request.height <= 0.0)
        {
            [view removeFromSuperview];
            return;
        }
        if (view == nil)
        {
            auto* const effect = [[NSVisualEffectView alloc] initWithFrame:NSZeroRect];
            effect.blendingMode = NSVisualEffectBlendingModeBehindWindow;
            effect.material = NSVisualEffectMaterialTitlebar;
            effect.state = NSVisualEffectStateFollowsWindowActiveState;
            effect.identifier = frostedBackdropIdentifier;
            [frame addSubview:effect positioned:NSWindowBelow relativeTo:content];
            view = effect;
        }
        // A strip across the window's top that follows its width.
        view.frame = topStripOf(frame, static_cast<CGFloat>(request.height));
        view.autoresizingMask = NSViewWidthSizable | (frame.isFlipped ? NSViewMaxYMargin : NSViewMinYMargin);
    }

    /// @return The colour @p rgba (0xAARRGGBB) names, or nil for 0.
    [[nodiscard]] NSColor* colorOf(uint32_t rgba)
    {
        if (rgba == 0)
            return nil;
        return [NSColor colorWithSRGBRed:static_cast<CGFloat>((rgba >> 16) & 0xFF) / 255.0
                                   green:static_cast<CGFloat>((rgba >> 8) & 0xFF) / 255.0
                                    blue:static_cast<CGFloat>(rgba & 0xFF) / 255.0
                                   alpha:static_cast<CGFloat>((rgba >> 24) & 0xFF) / 255.0];
    }

    /// Lays the tabs out as Liquid Glass (TabSurface::Glass), or takes them away.
    ///
    /// Apple's model: glass on the controls and navigation layer alone, in one NSGlassEffectContainerView so
    /// the shapes share one sampling region (glass cannot sample glass), the regular style because the tabs
    /// carry text, corners concentric with the tab, and colour spent on the one prominent element -- the
    /// active tab's tint. The container sits behind the Qt content, which paints the labels over the glass and
    /// keeps every click; the glass shapes therefore carry no content view of their own and are not
    /// interactive, since no pointer event reaches them.
    /// @return Unsupported where AppKit has no Liquid Glass (before macOS 26): the window then paints its tabs.
    [[nodiscard]] std::expected<void, TitleBandMaterialError> applyGlassTabs(NSWindow* window,
                                                                            TitleBandRequest const& request)
    {
        NSView* const content = window.contentView;
        NSView* const frame = content.superview;
        if (frame == nil)
            return {};

        NSView* container = subviewIdentified(frame, glassBackdropIdentifier);
        if (request.tabs != config::TabSurface::Glass)
        {
            [container removeFromSuperview];
            return {};
        }

        // Looked up by name, so the build needs no macOS 26 SDK.
        Class const glassClass = NSClassFromString(@"NSGlassEffectView");
        Class const containerClass = NSClassFromString(@"NSGlassEffectContainerView");
        if (glassClass == nil || containerClass == nil)
            return std::unexpected(TitleBandMaterialError::Unsupported);

        if (container == nil)
        {
            container = [[containerClass alloc] initWithFrame:frame.bounds];
            container.identifier = glassBackdropIdentifier;
            container.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
            // The shapes stay apart at rest, as the tabs do: merging is for shapes that move into one another.
            [container setValue:@0.0 forKey:@"spacing"];
            auto* const shapes = [[NSView alloc] initWithFrame:frame.bounds];
            shapes.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
            [container setValue:shapes forKey:@"contentView"];
            [frame addSubview:container positioned:NSWindowBelow relativeTo:content];
        }
        container.frame = frame.bounds;
        NSView* const shapes = [container valueForKey:@"contentView"];
        if (shapes == nil)
            return {};

        // One glass view per tab, reused by position: tabs come and go at the end far more than anywhere else.
        while (shapes.subviews.count > request.tabShapes.size())
            [shapes.subviews.lastObject removeFromSuperview];
        while (shapes.subviews.count < request.tabShapes.size())
            [shapes addSubview:[[glassClass alloc] initWithFrame:NSZeroRect]];

        auto const height = NSHeight(shapes.bounds);
        auto index = NSUInteger { 0 };
        for (auto const& shape: request.tabShapes)
        {
            NSView* const glass = shapes.subviews[index++];
            // The strip publishes window-relative coordinates from the top-left.
            glass.frame = shapes.isFlipped ? NSMakeRect(shape.x, shape.y, shape.width, shape.height)
                                           : NSMakeRect(shape.x, height - shape.y - shape.height, shape.width,
                                                        shape.height);
            [glass setValue:@(shape.radius) forKey:@"cornerRadius"];
            [glass setValue:colorOf(shape.tint) forKey:@"tintColor"];
        }
        return {};
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

    /// @return The title bar's height: windowed, Qt's safe-area top (the title bar above the content layout
    ///         rectangle); in full screen, where the safe area is empty, the bare title bar AppKit reveals over
    ///         the window's top in a window of its own (measured: 32 pt, the titled frame's extent).
    [[nodiscard]] qreal titleBarHeightOf(QWindow const& window, NSWindow* nsWindow)
    {
        if ((nsWindow.styleMask & NSWindowStyleMaskFullScreen) == 0)
            return static_cast<qreal>(window.safeAreaMargins().top());
        auto const content = NSMakeRect(0, 0, 100, 100);
        auto const frame = [NSWindow frameRectForContentRect:content styleMask:NSWindowStyleMaskTitled];
        return static_cast<qreal>(NSHeight(frame) - NSHeight(content));
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
    /// the screen's top (S5). The frosted band (`tab_style.material`) is the title bar's material in a view of
    /// ours behind the content view (applyBackdropView); the glass band draws each tab as Liquid Glass on macOS
    /// 26 (applyGlassTabs). The shadow is asserted again after every apply(), as §3.5 asks:
    /// AppKit derives it lazily from the window's alpha.
    class CocoaTitleBandMaterial final: public ITitleBandMaterial
    {
      public:
        /// @pre GUI (main) thread.
        /// @return Success: AppKit reports no failure to dress a title bar.
        [[nodiscard]] std::expected<void, TitleBandMaterialError> apply(QWindow& window,
                                                                        TitleBandRequest const& request) override
        {
            Require(NSThread.isMainThread);
            NSWindow* const nsWindow = nsWindowOf(window);
            if (nsWindow == nil)
                return {};

            // The title bar's own background is Qt's to clear (Qt::NoTitleBarBackgroundHint, set by the
            // controller); a blurred band adds a view of ours behind the content.
            nsWindow.appearance = appearanceFor(request.scheme);
            applyGrowth(nsWindow, request.growth);
            applyBackdropView(nsWindow, request);
            auto const tabs = applyGlassTabs(nsWindow, request);
            nsWindow.hasShadow = YES;
            [nsWindow invalidateShadow];
            return tabs;
        }

        /// @pre GUI (main) thread.
        [[nodiscard]] NativeTitleBarMetrics metrics(QWindow& window) const override
        {
            Require(NSThread.isMainThread);
            NSWindow* const nsWindow = nsWindowOf(window);
            if (nsWindow == nil)
                return {};
            return NativeTitleBarMetrics { .height = titleBarHeightOf(window, nsWindow),
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
