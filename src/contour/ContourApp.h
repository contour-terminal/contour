// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <crispy/App.h>

namespace contour
{

/// Contour CLI application base.
///
/// TODO: provide special installable targets in debian packages (cmake and PPA)
class ContourApp: public crispy::app
{
  public:
    ContourApp();

    [[nodiscard]] crispy::cli::command parameterDefinition() const override;

  private:
    int captureAction();
    int listDebugTagsAction();
    int parserTableAction();
    int profileAction();
    int terminfoAction();
    int configAction();
    int integrationAction();
    int infoVT();
    int documentationVT();
    int documentationKeyMapping();
    int documentationGlobalConfig();
    int documentationProfileConfig();
    /// Displays an image in the terminal via GIP oneshot sequence.
    int catAction();
    /// Runs the headless terminal multiplexer daemon.
    int daemonAction();
};

} // namespace contour
