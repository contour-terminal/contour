// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <crispy/App.h>

namespace contour
{

/// Contour TUI application.
///
/// TODO: provide special installable targets in debian packageS (cmake and PPA)
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
#if defined(GOOD_IMAGE_PROTOCOL)
    int imageAction();
#endif
};

} // namespace contour
