// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/ExternalLauncher.hpp>
#include <contour/platform/QtExternalLauncher.hpp>

#ifdef __linux__
    #include <contour/platform/PortalExternalLauncher.hpp>
#endif

#include <utility>

namespace contour::platform
{

std::unique_ptr<ExternalLauncher> makeExternalLauncher(std::shared_ptr<vtpty::ProcessPlacement> placement)
{
#ifdef __linux__
    return std::make_unique<PortalExternalLauncher>(
        qtPortalCaller(OpenUriPortalInterface), std::make_unique<QtExternalLauncher>(std::move(placement)));
#else
    return std::make_unique<QtExternalLauncher>(std::move(placement));
#endif
}

} // namespace contour::platform
