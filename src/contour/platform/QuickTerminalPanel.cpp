// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/QuickTerminalPanel.hpp>

// macOS defines makeQuickTerminalPanel() in CocoaQuickTerminalPanel.mm.
#ifndef __APPLE__

    #include <QtCore/QByteArray>
    #include <QtGui/QWindow>

    #include <memory>

namespace contour::platform
{

namespace
{
    /// The environment variable Qt hands to the compositor as the window's activation token.
    constexpr auto ActivationTokenVariable = "XDG_ACTIVATION_TOKEN";

    /// The panel of every host but macOS: an ordinary top-level window that is shown, raised and asked
    /// for the focus. A Wayland compositor only grants it with the activation token the portal's
    /// Activated signal carried, which Qt reads from the environment as the window is activated.
    class QtQuickTerminalPanel final: public IQuickTerminalPanel
    {
      public:
        /// Does nothing: nothing about the window differs from any other's.
        void adopt(QWindow& /*window*/) override {}

        /// Shows, raises and activates @p window.
        void present(QWindow& window, std::string const& activationToken) override
        {
            if (!activationToken.empty())
                qputenv(ActivationTokenVariable, QByteArray::fromStdString(activationToken));
            window.show();
            window.raise();
            window.requestActivate();
            if (!activationToken.empty())
                qunsetenv(ActivationTokenVariable);
        }

        /// Hides @p window.
        void dismiss(QWindow& window) override { window.hide(); }
    };
} // namespace

std::unique_ptr<IQuickTerminalPanel> makeQuickTerminalPanel()
{
    return std::make_unique<QtQuickTerminalPanel>();
}

} // namespace contour::platform

#endif
