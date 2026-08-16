// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace contour::platform
{

/// Who draws the window's title bar and frame.
///
/// One enum shared by the two decisions that turn on it -- which frame the operating system should
/// give the window (@see framePolicyFor) and whether the window has to publish a drop shadow of its
/// own (@see shadowVisibilityFor) -- because they are answering the same question about the same
/// window, and two spellings of it invite the two to disagree.
enum class WindowDecoration : uint8_t
{
    Client = 0, //!< Our tab strip is the whole decoration. The default.
    Server,     //!< The OS draws the title bar, its window controls, and its own drop shadow.
};

} // namespace contour::platform
