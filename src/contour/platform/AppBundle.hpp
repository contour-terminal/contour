// SPDX-License-Identifier: Apache-2.0
//
// Whether the process runs from an application bundle. Qt-free and header-only: the decision is a pure
// function of the two strings the Cocoa adapter reads from `NSBundle.mainBundle`, so it is table-tested
// in contour_test.
#pragma once

#include <cstdint>
#include <string_view>

namespace contour::platform
{

/// Whether the process runs from an application bundle that macOS can identify.
enum class AppBundle : uint8_t
{
    Absent = 0, ///< A bare binary, a test run, or `contour` from a shell.
    Present,    ///< An `.app` with a bundle identifier.
};

/// Whether a main bundle is one the user-notification centre may be asked for.
///
/// `+[UNUserNotificationCenter currentNotificationCenter]` raises outside an application bundle with an
/// identifier, so the class must never be touched unless this says Present. The path ends in `.app`
/// (case sensitive, after any trailing slashes) and the identifier is not empty.
/// @param bundlePath `NSBundle.mainBundle.bundlePath`.
/// @param bundleIdentifier `NSBundle.mainBundle.bundleIdentifier`, empty when it has none.
/// @return Present for an `.app` with an identifier, else Absent.
[[nodiscard]] constexpr AppBundle appBundleOf(std::string_view bundlePath,
                                              std::string_view bundleIdentifier) noexcept
{
    constexpr auto Suffix = std::string_view { ".app" };

    while (bundlePath.ends_with('/'))
        bundlePath.remove_suffix(1);

    return bundlePath.ends_with(Suffix) && !bundleIdentifier.empty() ? AppBundle::Present : AppBundle::Absent;
}

} // namespace contour::platform
