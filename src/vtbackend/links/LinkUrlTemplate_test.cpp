// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/links/LinkUrlTemplate.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

using namespace vtbackend;

namespace
{
/// The group reader expand() takes, over @p groups.
template <std::size_t N>
[[nodiscard]] auto groupsOf(std::array<std::string_view, N> const& groups)
{
    return [&groups](int index) {
        return groups.at(static_cast<std::size_t>(index));
    };
}
} // namespace

TEST_CASE("LinkUrlTemplate.expandsGroupsEncodingOnlyWhatAUrlCannotHold", "[links]")
{
    auto const groups = std::array<std::string_view, 2> { "ABC 1#2", "ABC" };
    auto const jira = LinkUrlTemplate::parse("https://jira.example.com/browse/{0}?project={1}");
    REQUIRE(jira);
    CHECK(jira->expand(groupsOf(groups)) == "https://jira.example.com/browse/ABC%201%232?project=ABC");
    CHECK(jira->highestGroup() == 1);

    // A query's reserved characters, and text beyond ASCII, go in as they are.
    auto const www = LinkUrlTemplate::parse("https://{0}");
    REQUIRE(www);
    auto const query = std::array<std::string_view, 1> { "www.example.com/a?b=1&c=ä" };
    CHECK(www->expand(groupsOf(query)) == "https://www.example.com/a?b=1&c=ä");
}

TEST_CASE("LinkUrlTemplate.aGroupThatTookNoPartInsertsNothing", "[links]")
{
    auto const groups = std::array<std::string_view, 3> { "ABC-1", "", "1" };
    auto const opener = LinkUrlTemplate::parse("https://j/{2}/{1}x");
    REQUIRE(opener);
    CHECK(opener->expand(groupsOf(groups)) == "https://j/1/x");
    CHECK(opener->highestGroup() == 2);
}

TEST_CASE("LinkUrlTemplate.aTemplateWithoutGroupsIsItsText", "[links]")
{
    auto const fixed = LinkUrlTemplate::parse("https://example.com/help");
    REQUIRE(fixed);
    CHECK(fixed->highestGroup() == -1);
    CHECK(fixed->expand([](int) { return std::string_view {}; }) == "https://example.com/help");
}

TEST_CASE("LinkUrlTemplate.knowsWhoChoseItsScheme", "[links]")
{
    // The user wrote the scheme only where the template starts with one, before any group (§7.6).
    auto const sourceOf = [](std::string_view text) {
        return LinkUrlTemplate::parse(text).value().schemeSource();
    };
    CHECK(sourceOf("https://jira.example/{0}") == LinkSchemeSource::Template);
    CHECK(sourceOf("myviewer:{0}") == LinkSchemeSource::Template);
    CHECK(sourceOf("{0}") == LinkSchemeSource::Printed);
    CHECK(sourceOf("{1}:{2}") == LinkSchemeSource::Printed);
    CHECK(sourceOf("jira/{0}") == LinkSchemeSource::Printed);
    CHECK(sourceOf("{0}https://x") == LinkSchemeSource::Printed);
}

TEST_CASE("LinkUrlTemplate.urlSchemeOf", "[links]")
{
    CHECK(urlSchemeOf("https://x.y/") == "https");
    CHECK(urlSchemeOf("ms-msdt:id") == "ms-msdt");
    CHECK(urlSchemeOf("svn+ssh://x") == "svn+ssh");
    CHECK(urlSchemeOf("C:/x") == "C"); // a drive letter reads as one, and no link may open it as printed
    CHECK(urlSchemeOf("report.pdf") == std::nullopt);
    CHECK(urlSchemeOf(":x") == std::nullopt);
    CHECK(urlSchemeOf("1x:y") == std::nullopt);
    CHECK(urlSchemeOf("a b:c") == std::nullopt);

    CHECK(hasUrlScheme("FILE:///x", "file"));
    CHECK(hasUrlScheme("fIlE://h/x", "file"));
    CHECK_FALSE(hasUrlScheme("files://x", "file"));
    CHECK_FALSE(hasUrlScheme("https://x", "file"));
}

TEST_CASE("LinkUrlTemplate.rejectsWhatIsNoGroup", "[links]")
{
    CHECK(LinkUrlTemplate::parse("https://x/{0").error() == LinkUrlTemplateError::Unterminated);
    CHECK(LinkUrlTemplate::parse("https://x/{id}").error() == LinkUrlTemplateError::NotAGroup);
    CHECK(LinkUrlTemplate::parse("https://x/{10}").error() == LinkUrlTemplateError::NotAGroup);
    CHECK(LinkUrlTemplate::parse("https://x/{}").error() == LinkUrlTemplateError::NotAGroup);
}

TEST_CASE("LinkUrlTemplate.percentEncode", "[links]")
{
    CHECK(percentEncodeForUrl("a b\"<>") == "a%20b%22%3C%3E");
    CHECK(percentEncodeForUrl("/?&=~%") == "/?&=~%");
    CHECK(percentEncodeForUrl("#[\\]^`{|}") == "%23%5B%5C%5D%5E%60%7B%7C%7D");
    CHECK(percentEncodeForUrl("\t\x7f") == "%09%7F");
}
