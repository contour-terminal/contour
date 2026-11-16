// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/tui/InputField.hpp>

#include <chrono>
#include <optional>
#include <string>

namespace core::tui
{

/// Confirms and accepts the currently displayed ghost text, suppressing the pending debounced
/// recompute so the consumed-prefix seed survives for a synchronous restore-on-backspace.
///
/// A prior keystroke may have left a re-prepended "guess" ghost whose debounced recompute has not
/// run yet. Recomputing here (cache-backed) replaces that guess with the real suggestion and clears
/// it if the completer no longer offers one — so we never commit a stale guess. If a ghost remains,
/// it is accepted and the pending recompute is disarmed: for the now-complete accepted text the
/// completer typically returns nothing, and the resulting clearGhostText() would otherwise wipe the
/// consumed-prefix seed that acceptGhostText() just set (defeating the synchronous restore).
///
/// @param inputField   The field whose ghost text is being accepted.
/// @param recompute    Callable that refreshes the ghost text for the current buffer (cache-backed).
/// @param dirty        The consumer's "ghost recompute pending" flag; cleared on accept.
/// @param pendingSince The consumer's debounce timestamp; reset on accept.
/// @return True if a ghost was present after recompute and was accepted; false if the recompute
///         left no ghost (nothing accepted) — the caller may then fall through to other handling.
template <typename Recompute>
[[nodiscard]] bool acceptGhostText(InputField& inputField,
                                   Recompute const& recompute,
                                   bool& dirty,
                                   std::optional<SteadyTimePoint>& pendingSince)
{
    recompute();
    if (!inputField.hasGhostText())
        return false;
    inputField.acceptGhostText();
    dirty = false;
    pendingSince.reset();
    return true;
}

/// Updates ghost text on an input field based on a suggest function.
///
/// Only shows ghost text when the cursor is at the end of the input.
/// Uses a cache to avoid repeated expensive completer calls for the same text.
///
/// @param inputField       The input field to update ghost text on.
/// @param suggestCacheText Cached input text from the last suggest call (updated in-place).
/// @param suggestCacheResult Cached suggestion result (updated in-place).
/// @param suggest          Callable: (string_view text, size_t cursor) -> optional<string>.
template <typename SuggestFn>
void updateGhostText(InputField& inputField,
                     std::string& suggestCacheText,
                     std::optional<std::string>& suggestCacheResult,
                     SuggestFn const& suggest)
{
    auto const text = inputField.text();
    auto const cursor = inputField.cursor();

    // Only show ghost text when cursor is at end of input.
    if (cursor != text.size())
    {
        inputField.clearGhostText();
        return;
    }

    // Check suggest cache — skip expensive completer call if text unchanged.
    if (text == suggestCacheText)
    {
        if (suggestCacheResult)
            inputField.setGhostText(*suggestCacheResult);
        else
            inputField.clearGhostText();
        return;
    }

    // Cache miss — call completer and store result.
    auto suggestion = suggest(text, cursor);
    suggestCacheText = std::string(text);
    suggestCacheResult = suggestion;

    if (suggestion)
        inputField.setGhostText(*suggestion);
    else
        inputField.clearGhostText();
}

} // namespace core::tui
