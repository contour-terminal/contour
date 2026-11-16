// SPDX-License-Identifier: Apache-2.0
#include <core/tui/completer/Completer.hpp>

#include <algorithm>
#include <ranges>

namespace core::tui::completer
{

void Completer::addProvider(std::unique_ptr<CompletionProvider> provider)
{
    _providers.push_back(std::move(provider));

    // Sort by priority (highest first), stably: providers share the default priority, and
    // gatherCompletions() drops a later duplicate by text, so an unstable sort would decide
    // which provider's item survives from the standard library's partitioning rather than from
    // the order the providers were registered in.
    std::ranges::stable_sort(_providers,
                             [](auto const& a, auto const& b) { return a->priority() > b->priority(); });
}

std::vector<CompletionItem> Completer::complete(std::string_view input, size_t cursorPosition) const
{
    auto results = gatherCompletions(input, cursorPosition);

    // Limit results
    if (results.size() > _config.maxSuggestions)
        results.resize(_config.maxSuggestions);

    return results;
}

std::optional<std::string> Completer::suggest(std::string_view input, size_t cursorPosition) const
{
    // For ghost text, we want the best single suggestion
    auto completions = gatherCompletions(input, cursorPosition);

    if (completions.empty())
        return std::nullopt;

    // Find the best completion that can provide a suffix
    for (auto const& item: completions)
    {
        // Check if the completion starts with the input up to cursor
        std::string_view const inputPrefix = input.substr(0, cursorPosition);
        if (item.text.starts_with(inputPrefix) && item.text.size() > inputPrefix.size())
            return item.text.substr(inputPrefix.size());
    }

    return std::nullopt;
}

void Completer::setConfig(CompletionConfig config)
{
    _config = config;
}

CompletionConfig const& Completer::config() const
{
    return _config;
}

size_t Completer::providerCount() const noexcept
{
    return _providers.size();
}

std::vector<CompletionItem> Completer::gatherCompletions(std::string_view input, size_t cursorPosition) const
{
    std::vector<CompletionItem> allResults;

    for (auto const& provider: _providers)
    {
        auto results = provider->complete(input, cursorPosition);
        for (auto& item: results)
        {
            // Avoid duplicates
            bool const isDuplicate = std::ranges::any_of(
                allResults, [&item](auto const& existing) { return existing.text == item.text; });

            if (!isDuplicate)
                allResults.push_back(std::move(item));
        }
    }

    // Sort by score (descending), then alphabetically
    std::ranges::sort(allResults, [](auto const& a, auto const& b) {
        if (a.score != b.score)
            return a.score > b.score;
        return a.text < b.text;
    });

    return allResults;
}

std::string Completer::findCommonPrefix(std::vector<CompletionItem> const& items)
{
    if (items.empty())
        return "";

    if (items.size() == 1)
        return items[0].text;

    std::string prefix = items[0].text;

    for (auto const i: std::views::iota(std::size_t { 1 }, std::max(std::size_t { 1 }, items.size())))
    {
        auto const& text = items[i].text;
        size_t j = 0;

        while (j < prefix.size() && j < text.size() && prefix[j] == text[j])
            ++j;

        prefix = prefix.substr(0, j);

        if (prefix.empty())
            break;
    }

    return prefix;
}

} // namespace core::tui::completer
