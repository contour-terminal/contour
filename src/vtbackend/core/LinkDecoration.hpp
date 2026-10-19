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

/// When links are decorated: hyperlink_decoration.mode, for OSC 8 hyperlinks and detected links alike.
enum class LinkDecorationMode : uint8_t
{
    Always = 0, ///< Always (the default).
    Hover,      ///< Only the link under the pointer.
    Modifier,   ///< Every link while the follow modifier is held.
    Never,      ///< Never; the follow gesture still opens them.
};

} // namespace vtbackend
