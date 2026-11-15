// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/core/Prompt.hpp>

#include <algorithm>
#include <expected>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>

#include <orchestrator/core/ArgumentKinds.hpp>
#include <orchestrator/core/Sanitize.hpp>

namespace orchestrator
{

namespace
{
    using namespace std::string_view_literals;

    /// The only scheme an item's URL may have.
    constexpr auto HttpsScheme = "https://"sv;

    /// Whether @p url is `https://`, an authority without userinfo, and one word of URL bytes.
    [[nodiscard]] constexpr bool isHttpsUrl(std::string_view url) noexcept
    {
        if (!url.starts_with(HttpsScheme) || !std::ranges::all_of(url, isUrlByte))
            return false;
        auto const rest = url.substr(HttpsScheme.size());
        auto const authority = rest.substr(0, rest.find_first_of("/?#"));
        return !authority.empty() && !authority.contains('@');
    }

    [[nodiscard]] constexpr PromptError promptErrorOf(NoteError error) noexcept
    {
        switch (error)
        {
            case NoteError::StartsWithDash: return PromptError::NoteStartsWithDash;
            case NoteError::TooLong: return PromptError::NoteTooLong;
        }
        return PromptError::NoteTooLong;
    }

    /// @p promptTemplate with every `{url}` replaced by @p url; a `{url}` inside @p url stays as it is.
    [[nodiscard]] std::string substituted(std::string_view promptTemplate, std::string_view url)
    {
        auto prompt = std::string {};
        auto separator = std::string_view {};
        for (auto const part: std::views::split(promptTemplate, UrlPlaceholder))
        {
            prompt += separator;
            prompt += std::string_view { part.begin(), part.end() };
            separator = url;
        }
        return prompt;
    }
} // namespace

std::expected<std::string, PromptError> buildPrompt(std::string_view promptTemplate,
                                                    std::string_view url,
                                                    std::optional<std::string_view> note)
{
    if (promptTemplate.contains('\0'))
        return std::unexpected { PromptError::TemplateNotText };
    if (!isHttpsUrl(url))
        return std::unexpected { PromptError::UrlNotHttps };

    auto prompt = substituted(promptTemplate, url);
    if (note)
    {
        auto const line = singleLineNote(*note);
        if (!line)
            return std::unexpected { promptErrorOf(line.error()) };
        if (!line->empty())
            prompt += std::format("\nNote: {}", *line);
    }

    // The guarantee: an accepted prompt is Text. A NUL was refused above, so a dash or the length remains.
    if (!argumentAccepts(ArgumentKind::Text, prompt))
        return std::unexpected { prompt.starts_with('-') ? PromptError::StartsWithDash
                                                         : PromptError::TooLong };
    return prompt;
}

} // namespace orchestrator
