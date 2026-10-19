// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace vtbackend
{

/// How a cell inside a link is decorated this frame. The renderer draws it with the profile's
/// hyperlink_decoration.normal or .hover decorator, in the colour scheme's hyperlink_decoration colours.
enum class LinkDecorationRole : uint8_t
{
    None = 0, ///< Not decorated.
    Normal,   ///< A link at rest.
    Hover,    ///< The link the pointer is over.
};

} // namespace vtbackend
