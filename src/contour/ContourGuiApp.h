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

#include <contour/ContourApp.h>

namespace contour
{

/// Extends ContourApp with terminal GUI capability.
class ContourGuiApp: public ContourApp
{
  public:
    ContourGuiApp();

    int run(int argc, char const* argv[]) override;
    crispy::cli::Command parameterDefinition() const override;

  private:
    int argc_ = 0;
    char const** argv_ = nullptr;
    int terminalGuiAction();
};

} // namespace contour
