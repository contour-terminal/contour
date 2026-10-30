// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/GlobalHotkey.hpp>

// macOS defines makeGlobalHotkey() in CocoaGlobalHotkey.mm, Windows in Win32GlobalHotkey.cpp and Linux in
// DBusGlobalShortcutsChannel.cpp; every other host answers with the null hotkey.
#if !defined(__APPLE__) && !defined(_WIN32) && !defined(__linux__)

namespace contour::platform
{

std::unique_ptr<IGlobalHotkey> makeGlobalHotkey()
{
    return std::make_unique<NullGlobalHotkey>();
}

} // namespace contour::platform

#endif
