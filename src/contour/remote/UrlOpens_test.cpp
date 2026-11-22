// SPDX-License-Identifier: Apache-2.0
#include <contour/remote/UrlOpens.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <vector>

TEST_CASE("UrlOpens: a window opens the web links a daemon asks for, and nothing else", "[remote][url-open]")
{
    auto const asked = std::vector<std::string> { "https://github.com/contour-terminal/contour/pull/2369",
                                                  "file:///etc/passwd",
                                                  "http://example.org/" };
    auto opened = std::vector<std::string> {};
    auto refused = std::vector<std::string> {};
    contour::remote::followUrlOpens(
        asked,
        [&](std::string_view url) { opened.emplace_back(url); },
        [&](std::string_view url, std::string_view why) {
            refused.push_back(std::string(url) + ": " + std::string(why));
        });
    CHECK(opened
          == std::vector<std::string> { "https://github.com/contour-terminal/contour/pull/2369",
                                        "http://example.org/" });
    CHECK(refused == std::vector<std::string> { "file:///etc/passwd: only http and https links open" });
}
