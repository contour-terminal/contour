// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/shell/BlockRail.hpp>

#include <vtbackend/core/ColorPalette.hpp>

namespace vtbackend
{

RGBColor railColor(ColorPalette const& palette,
                   CommandBlockOutcome outcome,
                   BlockAttention attention) noexcept
{
    return attention == BlockAttention::Rest ? palette.blockStatusColor(outcome)
                                             : palette.blockStatusHoverColor(outcome);
}

} // namespace vtbackend
