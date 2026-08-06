// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string_view>

namespace contour::platform
{

/// Puts @p data on the desktop's clipboard, as UTF-8 text.
///
/// The clipboard is an ambient, application-wide resource — hence platform/, not the terminal view that
/// happens to have triggered the copy. Silently does nothing when there is no clipboard (a headless
/// QGuiApplication-less run), which is the same thing the platform does when the user has no session.
///
/// GUI THREAD ONLY: QClipboard is not thread-safe, and callers on the terminal thread must hop first.
/// @param data The text to publish.
void copyToClipboard(std::string_view data);

} // namespace contour::platform
