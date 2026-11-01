// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QtCore/QString>

#include <string_view>

namespace contour::window
{

/// @param text UTF-8 text.
/// @return It as a QString.
[[nodiscard]] inline QString toQString(std::string_view text)
{
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

} // namespace contour::window
