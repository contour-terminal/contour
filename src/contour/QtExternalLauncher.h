// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/ExternalLauncher.h>

namespace contour
{

/// The production ExternalLauncher: forwards to Qt's desktop-integration statics
/// (QDesktopServices::openUrl, QProcess::startDetached/execute).
class QtExternalLauncher final: public ExternalLauncher
{
  public:
    [[nodiscard]] bool openUrl(QUrl const& url) override;
    bool runDetached(QString const& program, QStringList const& arguments) override;
    int execute(QString const& program, QStringList const& arguments) override;
};

} // namespace contour
