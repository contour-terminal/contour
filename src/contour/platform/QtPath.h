// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QtCore/QString>

#include <filesystem>

namespace contour::platform
{

/// @p path as a QString, decoded with the encoding std::filesystem actually stores it in.
///
/// The obvious spelling -- `QString::fromStdString(path.generic_string())` -- is wrong on Windows:
/// generic_string() narrows the native wide path through the ANSI code page, while fromStdString()
/// decodes UTF-8. A profile directory holding any non-ASCII character (`C:\\Users\\Jürgen\\...`) then
/// comes back full of replacement characters and names a directory that does not exist -- silently,
/// because nothing along the way is obliged to notice. The wide form has no lossy step on any
/// platform.
///
/// @param path The path to convert.
/// @return @p path in Qt's own string type, with generic ('/') separators.
[[nodiscard]] inline QString toQString(std::filesystem::path const& path)
{
    return QString::fromStdWString(path.generic_wstring());
}

} // namespace contour::platform
