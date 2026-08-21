// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/platform/ExternalLauncher.hpp>

namespace contour::platform
{

/// The production ExternalLauncher: forwards to Qt's desktop-integration statics
/// (QDesktopServices::openUrl, QProcess::startDetached/execute).
///
/// Used where no portal is reachable — every platform but Linux, and a Linux build without QtDBus.
/// On Linux with D-Bus, PortalExternalLauncher is used instead, because Qt's openUrl() reaches the
/// portal with a BLOCKING call. @see makeExternalLauncher().
class QtExternalLauncher final: public ExternalLauncher
{
  public:
    [[nodiscard]] std::expected<void, LaunchError> openUrl(QUrl const& url) override;
    bool runDetached(QString const& program, QStringList const& arguments) override;
    int execute(QString const& program, QStringList const& arguments) override;
};

} // namespace contour::platform
