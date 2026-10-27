// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/shell/BlockRail.hpp>

#include <vtbackend/core/ColorPalette.hpp>

namespace vtbackend
{

RGBColor railColor(ColorPalette const& palette,
                   CommandBlockOutcome outcome,
                   BlockAttention attention) noexcept
{
    // The scheme's status colours, lifted toward the page's ink where a scheme made them too faint for its
    // own background to read as a mark (one-dark's success, gruvbox-dark's failure): the rail is the block's
    // one sign in the gutter.
    auto const ground = palette.defaultBackground;
    auto const ink = legibleInkOn(palette, ground);
    auto const atRest = liftToContrast(palette.blockStatusColor(outcome), ink, ground, MinimumMarkContrast);
    if (attention == BlockAttention::Rest)
        return atRest;

    // Lit, it still has to look lit: where both lifts land on one colour, the lit one goes halfway on.
    auto const lit = liftToContrast(palette.blockStatusHoverColor(outcome), ink, ground, MinimumMarkContrast);
    return lit != atRest ? lit : mixColor(atRest, ink, 0.5F);
}

} // namespace vtbackend
