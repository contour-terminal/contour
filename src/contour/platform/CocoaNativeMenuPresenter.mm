// SPDX-License-Identifier: Apache-2.0
//
// The macOS native menu presenter (ruling R21): a MenuTree becomes an NSMenu built directly, not a QMenu,
// so the menu is AppKit's own -- its font, its check column, its keyboard navigation, its submenus.
#include <contour/platform/CocoaNativeMenu.hpp>
#include <contour/platform/NativeMenuPresenter.hpp>

#include <core/Assert.hpp>

#include <QtGui/QGuiApplication>
#include <QtGui/QWindow>

#include <cmath>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

#import <AppKit/AppKit.h>

/// The target of every command item of one menu: remembers the tag of the item that was picked.
@interface ContourMenuPickTarget: NSObject
@property(nonatomic) NSInteger pickedTag; ///< The picked item's tag; -1 until one is picked.
- (void)pick:(NSMenuItem*)sender;
@end

@implementation ContourMenuPickTarget
- (instancetype)init
{
    if ((self = [super init]))
        _pickedTag = -1;
    return self;
}

- (void)pick:(NSMenuItem*)sender
{
    self.pickedTag = sender.tag;
}
@end

namespace contour::platform
{
namespace
{
    using command::ContextMenuEntryKind;
    using command::MenuNode;
    using command::RowAvailability;
    using command::RowCheck;

    /// @return @p text as an NSString; empty if it is not valid UTF-8.
    [[nodiscard]] NSString* toNSString(std::string const& text)
    {
        NSString* const result = [[NSString alloc] initWithBytes:text.data()
                                                          length:text.size()
                                                        encoding:NSUTF8StringEncoding];
        return result != nil ? result : @"";
    }

    /// @return @p text as UTF-8.
    [[nodiscard]] std::string toStdString(NSString* text)
    {
        char const* const utf8 = text.UTF8String;
        return utf8 != nullptr ? std::string { utf8 } : std::string {};
    }

    /// @return A square of the menu font's line height, filled with @p color.
    [[nodiscard]] NSImage* swatchImage(vtbackend::RGBColor color)
    {
        NSFont* const font = [NSFont menuFontOfSize:0];
        auto const side = std::ceil(font.ascender - font.descender + font.leading);
        NSColor* const fill = [NSColor colorWithSRGBRed:color.red / 255.0
                                                  green:color.green / 255.0
                                                   blue:color.blue / 255.0
                                                  alpha:1.0];
        return [NSImage imageWithSize:NSMakeSize(side, side)
                              flipped:NO
                       drawingHandler:^BOOL(NSRect rect) {
                         [fill setFill];
                         NSRectFill(rect);
                         return YES;
                       }];
    }

    /// Builds the menu titled @p title for @p nodes; command items target @p picker.
    /// @pre Called on the GUI (main) thread.
    [[nodiscard]] NSMenu* buildMenu(std::span<MenuNode const> nodes,
                                    NSString* title,
                                    ContourMenuPickTarget* picker)
    {
        NSMenu* const menu = [[NSMenu alloc] initWithTitle:title];
        // The tree decides what is enabled; AppKit's validation would re-enable every item with a target.
        menu.autoenablesItems = NO;
        for (auto const& node: nodes)
        {
            if (node.kind == ContextMenuEntryKind::Separator)
            {
                NSMenuItem* const separator = NSMenuItem.separatorItem;
                separator.tag = -1;
                [menu addItem:separator];
                continue;
            }

            // No key equivalents (R-i): a context menu only names the chord in its title column, and
            // AppKit would otherwise fire it while focus is elsewhere.
            NSMenuItem* const item = [[NSMenuItem alloc] initWithTitle:toNSString(node.title)
                                                                action:nil
                                                         keyEquivalent:@""];
            item.enabled = node.availability == RowAvailability::Enabled;
            if (node.kind == ContextMenuEntryKind::Submenu)
            {
                item.tag = -1;
                item.submenu = buildMenu(node.children, item.title, picker);
            }
            else
            {
                item.tag = static_cast<NSInteger>(node.actionIndex);
                item.target = picker;
                item.action = @selector(pick:);
                item.state = node.check == RowCheck::Checked ? NSControlStateValueOn : NSControlStateValueOff;
                if (node.swatch)
                    item.image = swatchImage(*node.swatch);
            }
            [menu addItem:item];
        }
        return menu;
    }

    /// @return What @p menu's items show, top to bottom.
    [[nodiscard]] std::vector<NativeMenuOutline> outline(NSMenu* menu)
    {
        auto result = std::vector<NativeMenuOutline> {};
        for (NSMenuItem* item in menu.itemArray)
        {
            auto const kind = item.isSeparatorItem ? ContextMenuEntryKind::Separator
                              : item.hasSubmenu    ? ContextMenuEntryKind::Submenu
                                                   : ContextMenuEntryKind::Command;
            result.push_back(NativeMenuOutline {
                .kind = kind,
                .title = toStdString(item.title),
                .availability = item.enabled ? RowAvailability::Enabled : RowAvailability::Disabled,
                .check = item.state == NSControlStateValueOn ? RowCheck::Checked : RowCheck::None,
                .image = item.image != nil ? SwatchImage::Present : SwatchImage::Absent,
                .tag = static_cast<long>(item.tag),
                .children = item.hasSubmenu ? outline(item.submenu) : std::vector<NativeMenuOutline> {},
            });
        }
        return result;
    }

    /// Shows a MenuTree as an NSMenu at the anchor, through AppKit's own menu tracking loop.
    /// @pre Every call is on the GUI (main) thread: AppKit is main-thread only.
    class CocoaNativeMenuPresenter final: public INativeMenuPresenter
    {
      public:
        /// @pre Called on the GUI thread.
        [[nodiscard]] std::expected<void, MenuPresentError> present(command::MenuTree const& tree,
                                                                    MenuAnchor const& anchor,
                                                                    MenuPicked const& onPicked) override
        {
            Require(NSThread.isMainThread);
            // A window with no platform window yet has no view to open in, and winId() would create one.
            if (anchor.position && (anchor.window == nullptr || anchor.window->handle() == nullptr))
                return std::unexpected(MenuPresentError::NoWindow);
            // winId() is an NSView* only under the cocoa plugin (see CocoaWindowFrame.mm); anywhere else
            // -- offscreen, minimal -- messaging it, or popping a menu at all, would be wrong.
            if (QGuiApplication::platformName() != QStringLiteral("cocoa"))
                return std::unexpected(MenuPresentError::Unsupported);

            ContourMenuPickTarget* const picker = [[ContourMenuPickTarget alloc] init];
            NSMenu* const menu = buildMenu(tree.nodes, @"", picker);
            if (anchor.position)
            {
                // ARC forbids casting an integer straight to an object pointer, hence the void* hop.
                NSView* const view = (__bridge NSView*) reinterpret_cast<void*>(anchor.window->winId());
                // Qt's view is flipped, so window-logical coordinates are view coordinates.
                [menu popUpMenuPositioningItem:nil
                                    atLocation:NSMakePoint(anchor.position->x(), anchor.position->y())
                                        inView:view];
            }
            else
                [menu popUpMenuPositioningItem:nil atLocation:NSEvent.mouseLocation inView:nil];

            // The tracking loop has returned; the pick runs on a clean stack, outside it.
            if (picker.pickedTag >= 0 && onPicked)
                onPicked(static_cast<std::size_t>(picker.pickedTag));
            return {};
        }
    };
} // namespace

std::vector<NativeMenuOutline> outlineNativeMenu(command::MenuTree const& tree)
{
    Require(NSThread.isMainThread);
    ContourMenuPickTarget* const picker = [[ContourMenuPickTarget alloc] init];
    return outline(buildMenu(tree.nodes, @"", picker));
}

std::unique_ptr<INativeMenuPresenter> makeNativeMenuPresenter()
{
    return std::make_unique<CocoaNativeMenuPresenter>();
}

} // namespace contour::platform
