// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for expandTabLabel — the pure tab-label template expander. These are dependency-free
// (no Qt, no app types): expandTabLabel only parses a template via crispy::parse_interpolated_string
// and substitutes from a TabLabelContext, so it is tested here in isolation from the QAbstractListModel
// integration covered in TabListModel_test.

#include <contour/TabLabel.h>

#include <catch2/catch_test_macros.hpp>

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

TEST_CASE("expandTabLabel: an unknown placeholder expands to empty", "[contour][tablabel]")
{
    auto const ctx = TabLabelContext { .position = 3, .windowTitle = "bash" };
    // "before" and "after" are literal; "{Nope}" is unrecognized and contributes nothing.
    CHECK(expandTabLabel("before{Nope}after", ctx) == "beforeafter");
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

TEST_CASE("expandTabLabel: an unterminated brace expands to empty", "[contour][tablabel]")
{
    // crispy::parse_interpolated_string treats text after an unmatched '{' as one placeholder, but it
    // keeps the leading '{' in the parsed name (unlike a properly closed "{Name}"). So "{WindowTitle"
    // parses to a placeholder literally named "{WindowTitle", which matches nothing and drops to empty.
    auto const ctx = TabLabelContext { .position = 1, .windowTitle = "vim" };
    CHECK(expandTabLabel("{WindowTitle", ctx).empty());
}

TEST_CASE("expandTabLabel: a zero/negative position is rendered as-is", "[contour][tablabel]")
{
    // The caller always passes a 1-based position, but the expander must not assume it: it formats
    // whatever integer it is given.
    CHECK(expandTabLabel("{TabPosition}", TabLabelContext { .position = 0, .windowTitle = "" }) == "0");
}

TEST_CASE("expandTabLabel: doubled braces escape to literal braces", "[contour][tablabel]")
{
    // The fix for the "tab rename eats {...}" finding: a user who wants literal braces in a rename
    // doubles them. "{{" -> "{" and "}}" -> "}", so the braced text survives instead of being parsed
    // as an (unknown, dropped) placeholder.
    auto const ctx = TabLabelContext { .position = 5, .windowTitle = "vim" };

    SECTION("a fully-escaped token renders literally")
    {
        CHECK(expandTabLabel("task {{123}}", ctx) == "task {123}");
        CHECK(expandTabLabel("build {{beta}}", ctx) == "build {beta}");
    }

    SECTION("escaped braces and a live placeholder coexist")
    {
        // {{x}} is literal, {WindowTitle} still expands.
        CHECK(expandTabLabel("{{x}}: {WindowTitle}", ctx) == "{x}: vim");
    }

    SECTION("a single brace pair still expands a known placeholder")
    {
        // Escaping must not break the established template behavior.
        CHECK(expandTabLabel("{WindowTitle}", ctx) == "vim");
        CHECK(expandTabLabel("[{TabPosition}]", ctx) == "[5]");
    }
}
