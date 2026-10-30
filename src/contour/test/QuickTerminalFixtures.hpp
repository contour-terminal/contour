// SPDX-License-Identifier: Apache-2.0
#pragma once

// A quick-terminal panel that records into a struct, so no test reaches AppKit or asks a compositor for the
// focus.

#include <contour/platform/QuickTerminalPanel.hpp>

#include <QtGui/QWindow>

#include <string>
#include <vector>

namespace contour::test
{

/// A panel that records what it is asked, and shows, hides and activates the window as Qt's own panel does,
/// so a test sees the window's real visibility and focus.
class RecordingQuickTerminalPanel final: public platform::IQuickTerminalPanel
{
  public:
    /// How often adopt() was called.
    int adopts = 0;
    /// Every activation token present() was given, in order.
    std::vector<std::string> presents {};
    /// How often dismiss() was called.
    int dismisses = 0;

    /// Counts the call.
    void adopt(QWindow& /*window*/) override { ++adopts; }

    /// Records @p activationToken, then shows and activates @p window.
    void present(QWindow& window, std::string const& activationToken) override
    {
        presents.push_back(activationToken);
        window.show();
        window.requestActivate();
    }

    /// Counts the call, then hides @p window.
    void dismiss(QWindow& window) override
    {
        ++dismisses;
        window.hide();
    }
};

} // namespace contour::test
