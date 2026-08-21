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

    [[nodiscard]] std::expected<void, contour::platform::SpawnError> runDetached(
        QString const& program, QStringList const& arguments) override
    {
        detached.push_back({ program, arguments });
        if (detachedError)
            return std::unexpected(*detachedError);
        return {};
    }

    [[nodiscard]] std::expected<int, contour::platform::SpawnError> execute(
        QString const& program, QStringList const& arguments) override
    {
        executed.push_back({ program, arguments });
        if (executeError)
            return std::unexpected(*executeError);
        return 0;
    }

    std::vector<QUrl> openedUrls;
    std::vector<Execution> detached;
    std::vector<Execution> executed;

    /// What openUrl() fails with; empty means it accepts (set one to exercise the error path).
    std::optional<contour::platform::LaunchError> openUrlError;

    /// What runDetached() fails with; empty means the child started. Set one to reach the
    /// "not even the fallback would take it" path.
    std::optional<contour::platform::SpawnError> detachedError;

    /// What execute() fails with; empty means the child ran and exited 0.
    std::optional<contour::platform::SpawnError> executeError;
};

} // namespace contour::test
