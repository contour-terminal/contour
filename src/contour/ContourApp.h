/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <crispy/App.h>

namespace contour
{

/// Contour TUI application.
///
/// TODO: provide special installable targets in debian packageS (cmake and PPA)
class ContourApp: public crispy::App
{
  public:
    ContourApp();

    crispy::cli::Command parameterDefinition() const override;

  private:
    int captureAction();
    int listDebugTagsAction();
    int parserTableAction();
    int profileAction();
    int terminfoAction();
    int configAction();
    int integrationAction();
    int imageAction();
};

} // namespace contour
