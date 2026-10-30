// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/LauncherMenu.hpp>

#ifdef __APPLE__

    #include <QtWidgets/QMenu>

    #include <memory>
    #include <utility>
    #include <vector>

namespace contour::platform
{

namespace
{
    /// The menu on the Dock icon: Qt's QMenu, installed with QMenu::setAsDockMenu(), whose entries run
    /// through the application.
    class DockMenu final: public LauncherMenu
    {
      public:
        explicit DockMenu(LauncherRun const& run): _run(run) { _menu.setAsDockMenu(); }

        [[nodiscard]] command::LauncherSurface surface() const noexcept override
        {
            return command::LauncherSurface::DockMenu;
        }

        void set(std::span<command::LauncherEntry const> entries) override
        {
            _menu.clear();
            for (auto const& entry: entries)
                _menu.addAction(QString::fromStdString(entry.title), [this, entry] { _run(entry); });
        }

      private:
        QMenu _menu;
        LauncherRun _run;
    };
} // namespace

std::unique_ptr<LauncherMenu> makeDockMenu(LauncherRun const& run)
{
    return std::make_unique<DockMenu>(run);
}

} // namespace contour::platform

#endif
