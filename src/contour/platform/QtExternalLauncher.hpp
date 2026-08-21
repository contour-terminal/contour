// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/platform/ExternalLauncher.hpp>

namespace contour::platform
{

/// The production ExternalLauncher: forwards to Qt's desktop-integration statics
/// (QDesktopServices::openUrl, QProcess::startDetached/execute).
///
/// Used where there is no xdg-desktop-portal to speak to -- every platform but Linux. On Linux,
/// PortalExternalLauncher is used instead, because Qt's openUrl() reaches the portal with a BLOCKING
/// call. @see makeExternalLauncher(), and PortalExternalLauncher for why that is not a sandbox-only
/// concern.
///
/// Its runDetached()/execute() are also what PortalExternalLauncher delegates to: starting a child
/// process has nothing to do with the portal, and is identical on both paths.
class QtExternalLauncher final: public ExternalLauncher
{
  public:
    [[nodiscard]] std::expected<void, LaunchError> openUrl(QUrl const& url) override;
    bool runDetached(QString const& program, QStringList const& arguments) override;
    int execute(QString const& program, QStringList const& arguments) override;
};

} // namespace contour::platform
