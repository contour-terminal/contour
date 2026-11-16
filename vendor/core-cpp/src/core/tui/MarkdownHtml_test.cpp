// SPDX-License-Identifier: Apache-2.0
#include <core/tui/MarkdownHtml.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

using namespace core::tui;

// ============================================================================
// trimAscii / findCaseInsensitive
// ============================================================================

TEST_CASE("MarkdownHtml.trimAscii")
{
    CHECK(trimAscii("  hi  ") == "hi");
    CHECK(trimAscii("\t hi \r") == "hi");
    CHECK(trimAscii("hi") == "hi");
    CHECK(trimAscii("   ").empty());
    CHECK(trimAscii("").empty());
    CHECK(trimAscii("a b") == "a b"); // interior whitespace preserved
}

TEST_CASE("MarkdownHtml.findCaseInsensitive")
{
    CHECK(findCaseInsensitive("Hello World", "world") == 6);
    CHECK(findCaseInsensitive("Hello", "HELLO") == 0);
    CHECK(findCaseInsensitive("Hello", "xyz") == std::string_view::npos);
    CHECK(findCaseInsensitive("", "a") == std::string_view::npos);
    CHECK(findCaseInsensitive("a", "") == std::string_view::npos);
    CHECK(findCaseInsensitive("ab", "abc") == std::string_view::npos); // needle longer
}

// ============================================================================
// parseHtmlTag
// ============================================================================

TEST_CASE("MarkdownHtml.parseHtmlTag.simple_open")
{
    auto const tag = parseHtmlTag("<div>", 0);
    REQUIRE(tag.has_value());
    CHECK(tag->name == "div");
    CHECK_FALSE(tag->isClosing);
    CHECK(tag->attrs.empty());
    CHECK(tag->endPos == 5);
}

TEST_CASE("MarkdownHtml.parseHtmlTag.closing")
{
    auto const tag = parseHtmlTag("</DIV>", 0);
    REQUIRE(tag.has_value());
    CHECK(tag->name == "div"); // lowercased
    CHECK(tag->isClosing);
}

TEST_CASE("MarkdownHtml.parseHtmlTag.with_attributes")
{
    auto const tag = parseHtmlTag(R"(<div class="x" align='center'>rest)", 0);
    REQUIRE(tag.has_value());
    CHECK(tag->name == "div");
    CHECK(tag->attrs == R"( class="x" align='center')");
    CHECK(tag->endPos == 30);
}

TEST_CASE("MarkdownHtml.parseHtmlTag.quoted_gt_does_not_end_tag")
{
    // The '>' inside the quoted value must not terminate the tag.
    auto const tag = parseHtmlTag(R"(<img alt="a > b" src="x.png">)", 0);
    REQUIRE(tag.has_value());
    CHECK(tag->name == "img");
    CHECK(htmlAttr(tag->attrs, "alt") == "a > b");
    CHECK(htmlAttr(tag->attrs, "src") == "x.png");
}

TEST_CASE("MarkdownHtml.parseHtmlTag.rejects_non_tags")
{
    CHECK_FALSE(parseHtmlTag("div>", 0).has_value()); // no '<'
    CHECK_FALSE(parseHtmlTag("<>", 0).has_value());   // empty name
    CHECK_FALSE(parseHtmlTag("<div", 0).has_value()); // unterminated
    CHECK_FALSE(parseHtmlTag("", 0).has_value());
    CHECK_FALSE(parseHtmlTag("<div>", 99).has_value()); // pos past end
    CHECK_FALSE(parseHtmlTag("< div>", 0).has_value()); // space before name
}

TEST_CASE("MarkdownHtml.parseHtmlTag.at_offset")
{
    auto const tag = parseHtmlTag("text <b>bold", 5);
    REQUIRE(tag.has_value());
    CHECK(tag->name == "b");
    CHECK(tag->endPos == 8);
}

// ============================================================================
// htmlAttr
// ============================================================================

TEST_CASE("MarkdownHtml.htmlAttr.quote_styles")
{
    CHECK(htmlAttr(" align=\"center\"", "align") == "center");
    CHECK(htmlAttr(" align='center'", "align") == "center");
    CHECK(htmlAttr(" align=center", "align") == "center");
    CHECK(htmlAttr(" align = \"center\"", "align") == "center");
}

TEST_CASE("MarkdownHtml.htmlAttr.case_insensitive_name")
{
    CHECK(htmlAttr(" ALIGN=\"center\"", "align") == "center");
}

TEST_CASE("MarkdownHtml.htmlAttr.absent")
{
    CHECK_FALSE(htmlAttr(" class=\"x\"", "align").has_value());
    CHECK_FALSE(htmlAttr("", "align").has_value());
}

TEST_CASE("MarkdownHtml.htmlAttr.does_not_match_suffix_of_another_attribute")
{
    // "data-align" must not satisfy a lookup for "align".
    CHECK_FALSE(htmlAttr(" data-align=\"center\"", "align").has_value());
    CHECK_FALSE(htmlAttr(" xalign=\"center\"", "align").has_value());
}

TEST_CASE("MarkdownHtml.htmlAttr.finds_later_occurrence_after_suffix_miss")
{
    CHECK(htmlAttr(" data-align=\"left\" align=\"center\"", "align") == "center");
}

TEST_CASE("MarkdownHtml.htmlAttr.bare_attribute_without_value")
{
    CHECK_FALSE(htmlAttr(" align", "align").has_value());
    CHECK_FALSE(htmlAttr(" align ", "align").has_value());
}

TEST_CASE("MarkdownHtml.htmlAttr.unterminated_quote")
{
    CHECK_FALSE(htmlAttr(" align=\"center", "align").has_value());
}

TEST_CASE("MarkdownHtml.htmlAttr.unquoted_value_stops_at_slash")
{
    CHECK(htmlAttr(" width=200/", "width") == "200");
}

// ============================================================================
// parseHtmlAlign / findHtmlBlockTag
// ============================================================================

TEST_CASE("MarkdownHtml.parseHtmlAlign")
{
    CHECK(parseHtmlAlign("center") == HtmlAlign::Center);
    CHECK(parseHtmlAlign("CENTER") == HtmlAlign::Center);
    CHECK(parseHtmlAlign("left") == HtmlAlign::Left);
    CHECK(parseHtmlAlign("right") == HtmlAlign::Right);
    CHECK_FALSE(parseHtmlAlign("justify").has_value());
    CHECK_FALSE(parseHtmlAlign("").has_value());
}

TEST_CASE("MarkdownHtml.findHtmlBlockTag")
{
    REQUIRE(findHtmlBlockTag("div") != nullptr);
    CHECK(findHtmlBlockTag("div")->headingLevel == 0);
    CHECK_FALSE(findHtmlBlockTag("div")->alwaysCenters);

    REQUIRE(findHtmlBlockTag("center") != nullptr);
    CHECK(findHtmlBlockTag("center")->alwaysCenters);

    REQUIRE(findHtmlBlockTag("h3") != nullptr);
    CHECK(findHtmlBlockTag("h3")->headingLevel == 3);

    CHECK(findHtmlBlockTag("img") == nullptr);
    CHECK(findHtmlBlockTag("span") == nullptr);
    CHECK(findHtmlBlockTag("h7") == nullptr);
    CHECK(findHtmlBlockTag("") == nullptr);
}

// ============================================================================
// parseWidthPx
// ============================================================================

TEST_CASE("MarkdownHtml.parseWidthPx")
{
    CHECK(parseWidthPx("200") == 200);
    CHECK(parseWidthPx("200px") == 200);
    CHECK(parseWidthPx("1") == 1);

    CHECK_FALSE(parseWidthPx("50%").has_value()); // percentages mean auto-fit
    CHECK_FALSE(parseWidthPx("0").has_value());   // zero is not a width
    CHECK_FALSE(parseWidthPx("-5").has_value());
    CHECK_FALSE(parseWidthPx("abc").has_value());
    CHECK_FALSE(parseWidthPx("").has_value());
    CHECK_FALSE(parseWidthPx("px").has_value());
    CHECK_FALSE(parseWidthPx("12ab").has_value()); // trailing garbage
    CHECK_FALSE(parseWidthPx(" 12").has_value());
}

// ============================================================================
// detectStandaloneImage
// ============================================================================

TEST_CASE("MarkdownHtml.detectStandaloneImage.markdown_syntax")
{
    auto const image = detectStandaloneImage("![alt text](logo.png)");
    REQUIRE(image.has_value());
    CHECK(image->alt == "alt text");
    CHECK(image->src == "logo.png");
    CHECK_FALSE(image->widthPx.has_value());
}

TEST_CASE("MarkdownHtml.detectStandaloneImage.markdown_with_surrounding_whitespace")
{
    auto const image = detectStandaloneImage("   ![a](x.png)  ");
    REQUIRE(image.has_value());
    CHECK(image->src == "x.png");
}

TEST_CASE("MarkdownHtml.detectStandaloneImage.html_syntax")
{
    auto const image = detectStandaloneImage(R"(<img src="logo.png" width="200" alt="L">)");
    REQUIRE(image.has_value());
    CHECK(image->alt == "L");
    CHECK(image->src == "logo.png");
    REQUIRE(image->widthPx.has_value());
    CHECK(*image->widthPx == 200);
}

TEST_CASE("MarkdownHtml.detectStandaloneImage.html_without_alt_or_width")
{
    auto const image = detectStandaloneImage("<img src=\"logo.png\">");
    REQUIRE(image.has_value());
    CHECK(image->alt.empty());
    CHECK_FALSE(image->widthPx.has_value());
}

TEST_CASE("MarkdownHtml.detectStandaloneImage.commonmark_title_excluded_from_src")
{
    auto const image = detectStandaloneImage(R"(![shot](docs/shot.png "Screenshot"))");
    REQUIRE(image.has_value());
    CHECK(image->src == "docs/shot.png");
    CHECK(image->alt == "shot");
}

TEST_CASE("MarkdownHtml.detectStandaloneImage.angle_bracketed_src")
{
    auto const image = detectStandaloneImage("![a](<my logo.png>)");
    REQUIRE(image.has_value());
    CHECK(image->src == "my logo.png");
}

TEST_CASE("MarkdownHtml.detectStandaloneImage.rejects_non_standalone")
{
    CHECK_FALSE(detectStandaloneImage("text ![a](x.png)").has_value());
    CHECK_FALSE(detectStandaloneImage("![a](x.png) text").has_value());
    CHECK_FALSE(detectStandaloneImage("<img src=\"x.png\"> text").has_value());
    CHECK_FALSE(detectStandaloneImage("[a](x.png)").has_value()); // a link, not an image
    CHECK_FALSE(detectStandaloneImage("<div>").has_value());
    CHECK_FALSE(detectStandaloneImage("<img>").has_value()); // no src
    CHECK_FALSE(detectStandaloneImage("").has_value());
    CHECK_FALSE(detectStandaloneImage("   ").has_value());
    CHECK_FALSE(detectStandaloneImage("![a](x.png").has_value()); // unterminated
}

TEST_CASE("MarkdownHtml.detectStandaloneImage.empty_alt")
{
    auto const image = detectStandaloneImage("![](x.png)");
    REQUIRE(image.has_value());
    CHECK(image->alt.empty());
    CHECK(image->src == "x.png");
}

// ============================================================================
// translateInlineHtml
// ============================================================================

TEST_CASE("MarkdownHtml.translateInlineHtml.anchor_becomes_markdown_link")
{
    CHECK(translateInlineHtml(R"(<a href="https://x.com">Docs</a>)") == "[Docs](https://x.com)");
}

TEST_CASE("MarkdownHtml.translateInlineHtml.anchor_with_surrounding_text")
{
    CHECK(translateInlineHtml("see <a href=\"u\">here</a> now") == "see [here](u) now");
}

TEST_CASE("MarkdownHtml.translateInlineHtml.nested_markup_inside_anchor")
{
    // <b> and </b> both map to the ** delimiter, so the emphasis closes.
    CHECK(translateInlineHtml(R"(<a href="u"><b>hi</b></a>)") == "[**hi**](u)");
}

TEST_CASE("MarkdownHtml.translateInlineHtml.img_becomes_alt_text")
{
    CHECK(translateInlineHtml("<img src=\"x.png\" alt=\"Logo\">") == "Logo");
    CHECK(translateInlineHtml("<img src=\"x.png\">").empty());
}

TEST_CASE("MarkdownHtml.translateInlineHtml.emphasis_tags")
{
    CHECK(translateInlineHtml("<b>x") == "**x");
    CHECK(translateInlineHtml("<strong>x") == "**x");
    CHECK(translateInlineHtml("<i>x") == "*x");
    CHECK(translateInlineHtml("<em>x") == "*x");
}

TEST_CASE("MarkdownHtml.translateInlineHtml.unknown_tags_dropped")
{
    CHECK(translateInlineHtml("<span class=\"x\">hi</span>") == "hi");
}

TEST_CASE("MarkdownHtml.translateInlineHtml.plain_text_untouched")
{
    CHECK(translateInlineHtml("hello **world**") == "hello **world**");
}

TEST_CASE("MarkdownHtml.translateInlineHtml.lone_angle_bracket_preserved")
{
    CHECK(translateInlineHtml("a < b") == "a < b");
}

TEST_CASE("MarkdownHtml.translateInlineHtml.anchor_without_close_is_dropped")
{
    // No </a>: the open tag is dropped, the text survives.
    CHECK(translateInlineHtml("<a href=\"u\">text") == "text");
}
