// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/tui/completer/CompletionItem.hpp>
#include <core/tui/completer/CompletionProvider.hpp>
#include <core/tui/completer/FuzzyMatch.hpp>
#include <core/tui/completer/SmartCaseMatch.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace core::tui::completer
{

/// @brief Configuration for the completion system.
struct CompletionConfig
{
    size_t maxSuggestions = 50;         ///< Maximum completions to return.
    SmartCaseConfig smartCaseConfig {}; ///< Smart case matching bonuses.
    FuzzyConfig fuzzyConfig {};         ///< Fuzzy matching configuration.
    bool enableFuzzy = true;            ///< Whether to include fuzzy matches (non-prefix).
};

/// @brief Orchestrates completion providers and generates suggestions.
///
/// The Completer manages multiple CompletionProvider instances and combines
/// their results. It handles:
/// - Provider registration and priority ordering
/// - Deduplication of results
/// - Sorting by score
/// - Limiting result count
///
/// Usage:
/// @code
///   Completer completer;
///   completer.addProvider(std::make_unique<MyProvider>());
///
///   auto items = completer.complete("git comm", 8);
///   // items contains completions from all registered providers
/// @endcode
class Completer
{
  public:
    Completer() = default;
    ~Completer() = default;

    // Non-copyable but movable
    Completer(Completer const&) = delete;
    Completer& operator=(Completer const&) = delete;
    Completer(Completer&&) = default;
    Completer& operator=(Completer&&) = default;

    /// @brief Registers a completion provider.
    ///
    /// Providers are sorted by priority (higher = checked first).
    /// @param provider The provider to add.
    void addProvider(std::unique_ptr<CompletionProvider> provider);

    /// @brief Gets all completions for the current input state.
    /// @param input The input line.
    /// @param cursorPosition The cursor byte offset.
    /// @return List of completion items, sorted by score then alphabetically.
    [[nodiscard]] std::vector<CompletionItem> complete(std::string_view input, size_t cursorPosition) const;

    /// @brief Gets the best single suggestion for ghost text.
    ///
    /// Returns the suffix to append for the highest-scored completion
    /// that starts with the input up to the cursor position.
    ///
    /// @param input The input line.
    /// @param cursorPosition The cursor byte offset.
    /// @return The best suggestion suffix to append, or nullopt if none.
    [[nodiscard]] std::optional<std::string> suggest(std::string_view input, size_t cursorPosition) const;

    /// @brief Sets the completion configuration.
    void setConfig(CompletionConfig config);

    /// @brief Returns the current configuration.
    [[nodiscard]] CompletionConfig const& config() const;

    /// @brief Returns the number of registered providers.
    [[nodiscard]] size_t providerCount() const noexcept;

    /// @brief Finds the common prefix among completion items.
    /// @param items The completion items.
    /// @return The common prefix, or empty string if none.
    [[nodiscard]] static std::string findCommonPrefix(std::vector<CompletionItem> const& items);

  private:
    std::vector<std::unique_ptr<CompletionProvider>> _providers;
    CompletionConfig _config;

    /// @brief Gathers completions from all providers and deduplicates.
    [[nodiscard]] std::vector<CompletionItem> gatherCompletions(std::string_view input,
                                                                size_t cursorPosition) const;
};

} // namespace core::tui::completer
