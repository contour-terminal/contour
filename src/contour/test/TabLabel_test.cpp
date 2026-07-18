// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for expandTabLabel — the pure tab-label template expander. These are dependency-free
// (no Qt, no app types): expandTabLabel only parses a template via crispy::parse_interpolated_string
// and substitutes from a TabLabelContext, so it is tested here in isolation from the QAbstractListModel
// integration covered in TabListModel_test.

#include <contour/TabLabel.h>

#include <catch2/catch_test_macros.hpp>

using contour::abbreviateHomePath;
using contour::expandTabLabel;
using contour::TabLabelContext;

TEST_CASE("expandTabLabel: default template yields the window title verbatim", "[contour][tablabel]")
{
    auto const ctx = TabLabelContext { .position = 1, .windowTitle = "vim" };
    CHECK(expandTabLabel("{WindowTitle}", ctx) == "vim");
}

TEST_CASE("expandTabLabel: position and title combine with literal text", "[contour][tablabel]")
{
    auto const ctx = TabLabelContext { .position = 2, .windowTitle = "vim" };
    CHECK(expandTabLabel("{TabPosition}: {WindowTitle}", ctx) == "2: vim");
}

TEST_CASE("expandTabLabel: an unknown placeholder is echoed verbatim", "[contour][tablabel]")
{
    auto const ctx = TabLabelContext { .position = 3, .windowTitle = "bash" };
    // "before" and "after" are literal; "{Nope}" is unrecognized and is echoed exactly as typed rather
    // than dropped, so the user sees what they wrote (matching the status line's handling).
    CHECK(expandTabLabel("before{Nope}after", ctx) == "before{Nope}after");
}

TEST_CASE("expandTabLabel: a template with no placeholders is returned verbatim", "[contour][tablabel]")
{
    auto const ctx = TabLabelContext { .position = 1, .windowTitle = "ignored" };
    CHECK(expandTabLabel("plain label", ctx) == "plain label");
}

TEST_CASE("expandTabLabel: an empty template yields an empty string", "[contour][tablabel]")
{
    auto const ctx = TabLabelContext { .position = 1, .windowTitle = "vim" };
    CHECK(expandTabLabel("", ctx).empty());
}

TEST_CASE("expandTabLabel: a repeated placeholder is substituted each time", "[contour][tablabel]")
{
    auto const ctx = TabLabelContext { .position = 7, .windowTitle = "x" };
    CHECK(expandTabLabel("{TabPosition}-{TabPosition}", ctx) == "7-7");
}

TEST_CASE("expandTabLabel: placeholder flags/attributes are accepted but ignored", "[contour][tablabel]")
{
    // Tab labels are plain text; styling flags carried by a placeholder must not change the value.
    auto const ctx = TabLabelContext { .position = 1, .windowTitle = "vim" };
    CHECK(expandTabLabel("{WindowTitle:Bold,Color=#FF0000}", ctx) == "vim");
}

TEST_CASE("expandTabLabel: an unterminated brace is echoed verbatim", "[contour][tablabel]")
{
    // crispy::parse_interpolated_string treats text after an unmatched '{' as one placeholder, but it
    // keeps the leading '{' in the parsed name (unlike a properly closed "{Name}"). So "{WindowTitle"
    // parses to a placeholder literally named "{WindowTitle", which matches nothing and is echoed verbatim
    // (its `whole` slice runs to end-of-input), so the unterminated text survives instead of vanishing.
    auto const ctx = TabLabelContext { .position = 1, .windowTitle = "vim" };
    CHECK(expandTabLabel("{WindowTitle", ctx) == "{WindowTitle");
}

TEST_CASE("expandTabLabel: a zero/negative position is rendered as-is", "[contour][tablabel]")
{
    // The caller always passes a 1-based position, but the expander must not assume it: it formats
    // whatever integer it is given.
    CHECK(expandTabLabel("{TabPosition}", TabLabelContext { .position = 0, .windowTitle = "" }) == "0");
}

TEST_CASE("expandTabLabel: unknown placeholders echo verbatim", "[contour][tablabel]")
{
    // There is no brace escaping (dropped for backward compatibility). Instead, an unrecognized placeholder
    // is echoed verbatim — the user sees exactly what they typed — matching the status line's handling so
    // both surfaces behave identically. Known placeholders still expand.
    auto const ctx = TabLabelContext { .position = 5, .windowTitle = "vim" };

    SECTION("a doubled-brace token round-trips verbatim (unknown placeholder + trailing brace)")
    {
        // "{{123}}" -> placeholder "{{123}" (name "{{123", unknown -> echoed as "{{123}") then literal "}",
        // so the whole doubled-brace token is reproduced exactly as typed.
        CHECK(expandTabLabel("task {{123}}", ctx) == "task {{123}}");
        CHECK(expandTabLabel("build {{beta}}", ctx) == "build {{beta}}");
    }

    SECTION("an unknown placeholder and a live placeholder coexist")
    {
        // "{{x}}" -> "{{x}" (unknown, echoed) + "}" ; "{WindowTitle}" still expands.
        CHECK(expandTabLabel("{{x}}: {WindowTitle}", ctx) == "{{x}}: vim");
    }

    SECTION("a plain unknown placeholder is echoed verbatim, flags and all")
    {
        CHECK(expandTabLabel("{Bogus:Bold,Color=#FFFF00}", ctx) == "{Bogus:Bold,Color=#FFFF00}");
    }

    SECTION("a single brace pair expands a known placeholder")
    {
        CHECK(expandTabLabel("{WindowTitle}", ctx) == "vim");
        CHECK(expandTabLabel("[{TabPosition}]", ctx) == "[5]");
    }
}

// {{{ abbreviateHomePath — the tab hover tooltip's working-directory line

TEST_CASE("abbreviateHomePath.a path under home is abbreviated", "[contour][tablabel]")
{
    CHECK(abbreviateHomePath("/home/bob/projects/contour", "/home/bob") == "~/projects/contour");
}

TEST_CASE("abbreviateHomePath.the home directory itself is a bare tilde", "[contour][tablabel]")
{
    CHECK(abbreviateHomePath("/home/bob", "/home/bob") == "~");
}

TEST_CASE("abbreviateHomePath.a sibling that merely shares the prefix is untouched", "[contour][tablabel]")
{
    // Without a component-boundary check this becomes "~by": a path that reads as real and points
    // somewhere else entirely.
    CHECK(abbreviateHomePath("/home/bobby", "/home/bob") == "/home/bobby");
    CHECK(abbreviateHomePath("/home/bobby/src", "/home/bob") == "/home/bobby/src");
}

TEST_CASE("abbreviateHomePath.a path outside home is untouched", "[contour][tablabel]")
{
    CHECK(abbreviateHomePath("/etc/contour", "/home/bob") == "/etc/contour");
    CHECK(abbreviateHomePath("/", "/home/bob") == "/");
}

TEST_CASE("abbreviateHomePath.an unknown home abbreviates nothing", "[contour][tablabel]")
{
    // QDir::homePath() can come back empty; abbreviating against "" would turn every path into "~".
    CHECK(abbreviateHomePath("/home/bob/src", "") == "/home/bob/src");
}

TEST_CASE("abbreviateHomePath.an empty path stays empty", "[contour][tablabel]")
{
    // Empty means "no working directory known", and the tooltip drops the line entirely.
    CHECK(abbreviateHomePath("", "/home/bob").empty());
}

// }}}
