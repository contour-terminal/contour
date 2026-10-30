// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/MenuBarIntegration.hpp>

// macOS has its own integration, CocoaMenuBarIntegration.mm.
#ifndef __APPLE__

namespace contour::platform
{

std::unique_ptr<IMenuBarIntegration> makeMenuBarIntegration()
{
    return std::make_unique<NullMenuBarIntegration>();
}

} // namespace contour::platform

#endif
