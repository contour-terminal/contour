// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What a window attached to a remote host (`contour client --host`) shows of it, so it is never mistaken
/// for a local one. Pure and Qt-free: WindowController hands it to QML.

#include <format>
#include <string>
#include <string_view>

namespace contour::window
{

/// What stands between the host and the session in a remote window's title.
inline constexpr std::string_view RemoteTitleSeparator = " — ";

/// The OS window title: "{host} — {session}" for a remote window, the session title alone otherwise.
/// @param hostLabel The attached host's label; empty for a local window.
/// @param sessionTitle The active session's title; may be empty while the first session resolves.
/// @return The title; the host alone for a remote window whose session has no title yet.
[[nodiscard]] inline std::string remoteWindowTitle(std::string_view hostLabel, std::string_view sessionTitle)
{
    if (hostLabel.empty())
        return std::string { sessionTitle };
    if (sessionTitle.empty())
        return std::string { hostLabel };
    return std::format("{}{}{}", hostLabel, RemoteTitleSeparator, sessionTitle);
}

} // namespace contour::window
