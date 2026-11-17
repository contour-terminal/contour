// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QtCore/QString>
#include <QtCore/QStringList>

#include <span>
#include <string>
#include <string_view>

namespace contour::window
{

/// @param text UTF-8 text.
/// @return It as a QString.
[[nodiscard]] inline QString toQString(std::string_view text)
{
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

/// @param lines UTF-8 lines.
/// @return Them as a QStringList, in order.
[[nodiscard]] inline QStringList toQStringList(std::span<std::string const> lines)
{
    auto list = QStringList {};
    list.reserve(static_cast<qsizetype>(lines.size()));
    for (auto const& line: lines)
        list.append(toQString(line));
    return list;
}

} // namespace contour::window
