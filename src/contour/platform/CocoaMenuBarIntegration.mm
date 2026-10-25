// SPDX-License-Identifier: Apache-2.0
//
// The macOS menu bar integration: the Window menu becomes the one AppKit lists the application's windows
// in, and the Help menu the one it puts its help search field into.
#include <contour/platform/MenuBarIntegration.hpp>

#include <core/Assert.hpp>

#include <QtWidgets/QMenu>

#import <AppKit/AppKit.h>

namespace contour::platform
{
namespace
{
    /// Takes Qt's placeholder Windows menu out of @p next's menu bar, so that @p next can be the Windows
    /// menu.
    ///
    /// Qt gives a native menu bar a hidden Windows menu of its own ("QtWindowMenu", see
    /// QCocoaMenuBar::insertWindowMenu()) when NSApp has none. AppKit answers NSApp.windowsMenu with the
    /// first menu of the main menu named as the Windows menu, so while the placeholder stays, it goes on
    /// listing the windows, and the menu handed over never does. Only a hidden item of @p next's own bar is
    /// taken out: a Windows menu some other bar shows is left where it is. The placeholder, its name and its
    /// being hidden were verified against Qt 6.11.1's cocoa plugin.
    /// @param previous The Windows menu in force.
    /// @param next The menu that is to be it.
    void retirePlaceholder(NSMenu* previous, NSMenu* next)
    {
        if (previous == nil || previous == next || previous.supermenu == nil
            || previous.supermenu != next.supermenu)
            return;
        NSMenu* const bar = previous.supermenu;
        auto const index = [bar indexOfItemWithSubmenu:previous];
        if (index >= 0 && [bar itemAtIndex:index].hidden)
            [bar removeItemAtIndex:index];
    }

    /// Hands the menus to NSApp.
    class CocoaMenuBarIntegration final: public IMenuBarIntegration
    {
      public:
        /// @pre Main thread: AppKit is main-thread only.
        void adopt(QMenu& windows, QMenu& help) override
        {
            Require(NSThread.isMainThread);
            // toNSMenu() is nil off the cocoa platform plugin; such a menu is left to Qt.
            if (NSMenu* const menu = windows.toNSMenu(); menu != nil)
            {
                retirePlaceholder(NSApp.windowsMenu, menu);
                NSApp.windowsMenu = menu;
            }
            if (NSMenu* const menu = help.toNSMenu(); menu != nil)
                NSApp.helpMenu = menu;
        }
    };
} // namespace

std::unique_ptr<IMenuBarIntegration> makeMenuBarIntegration()
{
    return std::make_unique<CocoaMenuBarIntegration>();
}

} // namespace contour::platform
