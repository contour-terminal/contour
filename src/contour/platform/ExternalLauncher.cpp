// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/ExternalLauncher.hpp>
#include <contour/platform/QtExternalLauncher.hpp>

#ifdef __linux__
    #include <contour/platform/PortalExternalLauncher.hpp>
#endif

namespace contour::platform
{

std::unique_ptr<ExternalLauncher> makeExternalLauncher()
{
#ifdef __linux__
    return std::make_unique<PortalExternalLauncher>(qtPortalCaller(OpenUriPortalInterface),
                                                    std::make_unique<QtExternalLauncher>());
#else
    return std::make_unique<QtExternalLauncher>();
#endif
}

} // namespace contour::platform
