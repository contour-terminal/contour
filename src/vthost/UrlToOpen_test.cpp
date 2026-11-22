// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>

#include <vthost/UrlToOpen.hpp>

using vthost::checkUrlToOpen;
using vthost::UrlRefusal;

namespace
{
/// One link and what the rule says of it.
struct UrlCase
{
    std::string_view url;
    std::expected<void, UrlRefusal> verdict;
};
} // namespace

TEST_CASE("url-open takes http and https links that name a host, and nothing else", "[vthost][url-open]")
{
    auto const cases = {
        UrlCase { .url = "https://github.com/contour-terminal/contour/pull/2369", .verdict = {} },
        UrlCase { .url = "http://example.org", .verdict = {} },
        UrlCase { .url = "HTTPS://example.org/a?b=c#d", .verdict = {} },
        UrlCase { .url = "file:///etc/passwd", .verdict = std::unexpected(UrlRefusal::NotWeb) },
        UrlCase { .url = "javascript://alert(1)", .verdict = std::unexpected(UrlRefusal::NotWeb) },
        UrlCase { .url = "example.org", .verdict = std::unexpected(UrlRefusal::NotWeb) },
        UrlCase { .url = "httpss://example.org", .verdict = std::unexpected(UrlRefusal::NotWeb) },
        UrlCase { .url = "https://", .verdict = std::unexpected(UrlRefusal::NoHost) },
        UrlCase { .url = "https:///path", .verdict = std::unexpected(UrlRefusal::NoHost) },
        UrlCase { .url = "https://a.org/x y", .verdict = std::unexpected(UrlRefusal::Disguised) },
        UrlCase { .url = "https://a.org/\x1b]8;;", .verdict = std::unexpected(UrlRefusal::Disguised) },
        UrlCase { .url = "https://a.org/\x7f", .verdict = std::unexpected(UrlRefusal::Disguised) },
    };
    for (auto const& [url, verdict]: cases)
    {
        CAPTURE(url);
        CHECK(checkUrlToOpen(url) == verdict);
    }
}

TEST_CASE("url-open takes a link of at most 2 KiB", "[vthost][url-open]")
{
    auto const prefix = std::string { "https://example.org/" };
    auto const longest = prefix + std::string(vthost::MaxUrlToOpenBytes - prefix.size(), 'a');
    CHECK(checkUrlToOpen(longest).has_value());
    CHECK(checkUrlToOpen(longest + "a")
          == std::expected<void, UrlRefusal> { std::unexpect, UrlRefusal::TooLong });
    CHECK(vthost::wordsOf(UrlRefusal::NotWeb) == "only http and https links open");
}
