// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The ExternalLauncher test double. Split out of GuiTestFixtures.hpp so a test that needs only
/// this one -- PortalExternalLauncher_test, which has no session, window or app to build -- does not
/// pull in the whole GUI fixture set to get it.

#include <contour/platform/ExternalLauncher.hpp>

#include <QtCore/QStringList>
#include <QtCore/QUrl>

#include <expected>
#include <optional>
#include <vector>

namespace contour::test
{

/// Records every URL-open / process-spawn request instead of launching it, so tests can assert the
/// routing and validation of the open-document, follow-hyperlink, open-configuration/-file-manager
/// /-selection, and spawn-new-terminal actions without touching the desktop.
///
/// It is also what PortalExternalLauncher's process half is injected with: its xdg-open fallback
/// would otherwise really spawn xdg-open, opening a browser at whoever ran the suite.
class RecordingExternalLauncher final: public contour::platform::ExternalLauncher
{
  public:
    struct Execution
    {
        QString program;
        QStringList arguments;
    };

    [[nodiscard]] std::expected<void, contour::platform::LaunchError> openUrl(QUrl const& url) override
    {
        openedUrls.push_back(url);
        if (openUrlError)
            return std::unexpected(*openUrlError);
        return {};
    }

    bool runDetached(QString const& program, QStringList const& arguments) override
    {
        detached.push_back({ program, arguments });
        return detachedSucceeds;
    }

    int execute(QString const& program, QStringList const& arguments) override
    {
        executed.push_back({ program, arguments });
        return 0;
    }

    std::vector<QUrl> openedUrls;
    std::vector<Execution> detached;
    std::vector<Execution> executed;

    /// What openUrl() fails with; empty means it accepts (set one to exercise the error path).
    std::optional<contour::platform::LaunchError> openUrlError;

    /// What runDetached() answers. Cleared to reach the "not even the fallback would take it" path.
    bool detachedSucceeds = true;
};

} // namespace contour::test
