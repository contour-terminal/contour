// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/links/LinkDetectionPolicy.hpp>
#include <vtbackend/screen/PrintedLinks.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace vtbackend;

TEST_CASE("LinkDetectionPolicy.defaultsDetectPathsOnThePrimaryScreenOnly", "[links]")
{
    constexpr auto Policy = LinkDetectionPolicy {};
    STATIC_CHECK(Policy.forPage(PageIndex(0)).detects(LinkCategory::Url));
    STATIC_CHECK(Policy.forPage(PageIndex(0)).detects(LinkCategory::FilePath));
    for (auto const page: { 1, 2, 15 })
    {
        CHECK(Policy.forPage(PageIndex(page)).detects(LinkCategory::Url));
        CHECK_FALSE(Policy.forPage(PageIndex(page)).detects(LinkCategory::FilePath));
    }
    CHECK(Policy.detectsAnywhere());
}

TEST_CASE("LinkDetectionPolicy.everythingOffDetectsNowhere", "[links]")
{
    constexpr auto Off = ScreenLinkDetection { .urls = DetectLinks::No, .filePaths = DetectLinks::No };
    constexpr auto Policy = LinkDetectionPolicy { .primaryScreen = Off, .otherScreens = Off };
    CHECK_FALSE(Policy.detectsAnywhere());
    for (auto const page: { 0, 1 })
        CHECK_FALSE(Policy.forPage(PageIndex(page)).detectsAny());
}

TEST_CASE("LinkDetectionPolicy.categories", "[links]")
{
    CHECK(linkCategoryOf(LinkKind::Url, "https://x.y/") == LinkCategory::Url);
    CHECK(linkCategoryOf(LinkKind::Custom, "JIRA-1") == LinkCategory::Url);
    CHECK(linkCategoryOf(LinkKind::Path, "src/a.cpp") == LinkCategory::FilePath);
    CHECK(linkCategoryOf(LinkKind::Url, "file:///etc/hosts") == LinkCategory::FilePath);
    CHECK(linkCategoryOf(LinkKind::Url, "FILE://host/x") == LinkCategory::FilePath);
}
