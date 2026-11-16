// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/tui/completer/CompletionItem.hpp>

#include <string_view>
#include <vector>

namespace core::tui::completer
{

/// @brief Abstract base for completion providers.
///
/// Completion providers generate suggestions based on input text and cursor position.
/// Multiple providers can be registered with a Completer orchestrator, which combines
/// their results and handles deduplication and sorting.
///
/// Implementations should be efficient, as completion may be triggered on every keystroke.
/// Consider caching results for expensive operations (e.g., PATH scanning).
class CompletionProvider
{
  public:
    virtual ~CompletionProvider() = default;

    /// @brief Generates completions for the given input at the cursor position.
    /// @param input The full input text.
    /// @param cursorPosition The cursor byte offset in the input.
    /// @return List of completion items (may be empty if not applicable).
    [[nodiscard]] virtual std::vector<CompletionItem> complete(std::string_view input,
                                                               size_t cursorPosition) = 0;

    /// @brief Returns the priority of this provider (higher = checked first).
    ///
    /// When multiple providers return results, higher-priority providers' items
    /// appear first in the combined list. Use priorities to control ordering
    /// between different completion sources (e.g., history vs filesystem).
    ///
    /// @return Priority value (default: 0).
    [[nodiscard]] virtual int priority() const { return 0; }
};

} // namespace core::tui::completer
