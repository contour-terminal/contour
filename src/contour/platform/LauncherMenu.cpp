// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/LauncherMenu.hpp>

namespace contour::platform
{

#ifdef __APPLE__
[[nodiscard]] std::unique_ptr<LauncherMenu> makeDockMenu(LauncherRun const& run);
#elifdef _WIN32
[[nodiscard]] std::unique_ptr<LauncherMenu> makeJumpList(std::string const& programPath);
#endif

std::unique_ptr<LauncherMenu> makeLauncherMenu([[maybe_unused]] std::string const& programPath,
                                               [[maybe_unused]] LauncherRun const& run)
{
#ifdef __APPLE__
    return makeDockMenu(run);
#elifdef _WIN32
    return makeJumpList(programPath);
#else
    return nullptr;
#endif
}

} // namespace contour::platform
