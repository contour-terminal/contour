// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <orchestrator/core/ArgumentKinds.hpp>
#include <orchestrator/core/Prompt.hpp>
#include <orchestrator/core/Sanitize.hpp>

namespace
{

using orchestrator::PromptError;

/// What a row expects of buildPrompt.
enum class PromptExpectation : uint8_t
{
    Refused,
    Accepted,
};

struct PromptRow
{
    std::string_view promptTemplate;
    std::string_view url;
    std::optional<std::string_view> note;
    PromptExpectation expectation;
    std::string_view expected; ///< The prompt, when accepted.
    PromptError error;         ///< The reason, when refused.
};

constexpr auto Pull = std::string_view { "https://github.com/o/r/pull/1" };

/// The array's characters as a string_view.
template <std::size_t N>
constexpr auto viewOf(std::array<char, N> const& text)
{
    return std::string_view { text.data(), text.size() };
}

/// @p Count copies of 'x'.
template <std::size_t Count>
constexpr auto xs()
{
    auto text = std::array<char, Count> {};
    text.fill('x');
    return text;
}

/// A URL and a template that together are exactly MaxTextBytes: "https://" + 'x's + " {url}".
constexpr auto LongestUrlTail = xs<orchestrator::MaxTextBytes - 8 - 2>();
constexpr auto LongNote = xs<orchestrator::MaxNoteChars + 1>();
/// A URL holding a NUL, spelled as bytes.
constexpr auto UrlWithNul = std::array { 'h', 't', 't', 'p', 's', ':', '/', '/', 'a', '\0', 'b' };

} // namespace

TEST_CASE("Prompt", "[orchestrator][prompt]")
{
    auto const longestUrl = std::string { "https://" } + std::string { viewOf(LongestUrlTail) };
    auto const longestPrompt = std::string { "x " } + longestUrl;

    auto const rows = std::array {
        PromptRow { .promptTemplate = "/x:fix-ci {url}",
                    .url = Pull,
                    .note = std::nullopt,
                    .expectation = PromptExpectation::Accepted,
                    .expected = "/x:fix-ci https://github.com/o/r/pull/1",
                    .error = {} },
        // An '@' past the authority is a path's, not userinfo.
        PromptRow { .promptTemplate = "fix {url}",
                    .url = "https://h.example/u@x?q=a@b#c@d",
                    .note = std::nullopt,
                    .expectation = PromptExpectation::Accepted,
                    .expected = "fix https://h.example/u@x?q=a@b#c@d",
                    .error = {} },
        PromptRow { .promptTemplate = "fix {url}",
                    .url = "https://h.example",
                    .note = std::nullopt,
                    .expectation = PromptExpectation::Accepted,
                    .expected = "fix https://h.example",
                    .error = {} },
        PromptRow { .promptTemplate = "/x:fix-ci {url}",
                    .url = Pull,
                    .note = "keep the order",
                    .expectation = PromptExpectation::Accepted,
                    .expected = "/x:fix-ci https://github.com/o/r/pull/1\nNote: keep the order",
                    .error = {} },
        PromptRow { .promptTemplate = "/x:fix-ci {url}",
                    .url = Pull,
                    .note = "  keep\r\nit  ",
                    .expectation = PromptExpectation::Accepted,
                    .expected = "/x:fix-ci https://github.com/o/r/pull/1\nNote: keep it",
                    .error = {} },
        // A note that sanitises to nothing adds no line.
        PromptRow { .promptTemplate = "/x:fix-ci {url}",
                    .url = Pull,
                    .note = " \r\n ",
                    .expectation = PromptExpectation::Accepted,
                    .expected = "/x:fix-ci https://github.com/o/r/pull/1",
                    .error = {} },
        // Every {url} is replaced, and a {url} inside the URL is not replaced again.
        PromptRow { .promptTemplate = "review {url}, then fix {url}",
                    .url = "https://h/{url}",
                    .note = std::nullopt,
                    .expectation = PromptExpectation::Accepted,
                    .expected = "review https://h/{url}, then fix https://h/{url}",
                    .error = {} },
        PromptRow { .promptTemplate = "x {url}",
                    .url = longestUrl,
                    .note = std::nullopt,
                    .expectation = PromptExpectation::Accepted,
                    .expected = longestPrompt,
                    .error = {} },
        PromptRow { .promptTemplate = "xx {url}",
                    .url = longestUrl,
                    .note = std::nullopt,
                    .expectation = PromptExpectation::Refused,
                    .expected = {},
                    .error = PromptError::TooLong },
        PromptRow { .promptTemplate = "x {url}",
                    .url = longestUrl,
                    .note = "n",
                    .expectation = PromptExpectation::Refused,
                    .expected = {},
                    .error = PromptError::TooLong },
        PromptRow { .promptTemplate = "-p {url}",
                    .url = Pull,
                    .note = std::nullopt,
                    .expectation = PromptExpectation::Refused,
                    .expected = {},
                    .error = PromptError::StartsWithDash },
        PromptRow { .promptTemplate = "fix {url}",
                    .url = Pull,
                    .note = "-p evil",
                    .expectation = PromptExpectation::Refused,
                    .expected = {},
                    .error = PromptError::NoteStartsWithDash },
        PromptRow { .promptTemplate = "fix {url}",
                    .url = Pull,
                    .note = viewOf(LongNote),
                    .expectation = PromptExpectation::Refused,
                    .expected = {},
                    .error = PromptError::NoteTooLong },
    };

    SECTION("accepted and refused prompts")
    {
        for (auto const& row: rows)
        {
            CAPTURE(row.promptTemplate, row.url, row.note, row.expectation, row.expected, row.error);
            auto const prompt = orchestrator::buildPrompt(row.promptTemplate, row.url, row.note);
            REQUIRE(prompt.has_value() == (row.expectation == PromptExpectation::Accepted));
            if (prompt)
            {
                CHECK(*prompt == row.expected);
                CHECK(orchestrator::argumentAccepts(orchestrator::ArgumentKind::Text, *prompt));
            }
            else
                CHECK(prompt.error() == row.error);
        }
    }

    SECTION("URLs that are refused")
    {
        constexpr auto Urls = std::array<std::string_view, 20> {
            "https:///x",
            "https://?",
            "https://#frag",
            "https://u:p@evil.example/",
            "https://evil.example@github.com/o/r",
            "http://github.com/o/r/pull/1",
            "HTTPS://github.com/o/r/pull/1",
            "ftp://github.com",
            "https://",
            "",
            "github.com/o/r/pull/1",
            "https://github.com/o r",
            "https://github.com/o\tr",
            "https://github.com/o\nr",
            "https://github.com/o\x1b]0;x\x07",
            "https://github.com/o\x7fr",
            "https://github.com/o\"r",
            "https://github.com/o'r",
            "https://github.com/o`r",
            "https://github.com/ä",
        };
        for (auto const url: Urls)
        {
            CAPTURE(url);
            auto const prompt = orchestrator::buildPrompt("fix {url}", url, std::nullopt);
            REQUIRE_FALSE(prompt.has_value());
            CHECK(prompt.error() == PromptError::UrlNotHttps);
        }
        auto const nul = orchestrator::buildPrompt("fix {url}", viewOf(UrlWithNul), std::nullopt);
        REQUIRE_FALSE(nul.has_value());
        CHECK(nul.error() == PromptError::UrlNotHttps);
    }

    SECTION("a template that is not Text is refused")
    {
        constexpr auto Template = std::array { 'f', 'i', 'x', '\0', ' ', '{', 'u', 'r', 'l', '}' };
        auto const prompt = orchestrator::buildPrompt(viewOf(Template), Pull, std::nullopt);
        REQUIRE_FALSE(prompt.has_value());
        CHECK(prompt.error() == PromptError::TemplateNotText);
    }
}
