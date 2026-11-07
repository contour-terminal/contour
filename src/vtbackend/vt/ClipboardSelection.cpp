// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/vt/ClipboardSelection.hpp>

#include <algorithm>
#include <array>

namespace vtbackend::clipboard_selection
{

namespace
{
    /// A letter of _Pc_ that names a place Contour can copy to.
    struct Letter
    {
        char letter;
        ClipboardTarget target;
    };

    /// The letters with a target, the canonical spelling of each target first: format() writes the first row
    /// it finds for a target. A letter absent here is skipped (see parse()).
    constexpr auto Letters = std::array {
        Letter { .letter = 'c', .target = ClipboardTarget::Clipboard },
        Letter { .letter = 'p', .target = ClipboardTarget::PrimarySelection },
        Letter { .letter = 's', .target = ClipboardTarget::Clipboard },
    };

    /// What an empty _Pc_ means: xterm's `s0`, and `s` is the clipboard.
    constexpr auto EmptySelection = ClipboardTarget::Clipboard;
} // namespace

std::optional<ClipboardTargets> parse(std::string_view pc)
{
    if (pc.empty())
        return ClipboardTargets { EmptySelection };

    auto targets = ClipboardTargets {};
    for (auto const ch: pc)
        if (auto const row = std::ranges::find(Letters, ch, &Letter::letter); row != Letters.end())
            targets.enable(row->target);

    if (targets.none())
        return std::nullopt;
    return targets;
}

std::string format(ClipboardTargets targets)
{
    auto result = std::string {};
    auto remaining = targets;
    for (auto const& row: Letters)
    {
        if (remaining.test(row.target))
        {
            result += row.letter;
            remaining.disable(row.target);
        }
    }
    return result;
}

} // namespace vtbackend::clipboard_selection
