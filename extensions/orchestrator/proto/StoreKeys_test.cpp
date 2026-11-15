// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <optional>
#include <string>
#include <string_view>

#include <orchestrator/proto/StoreKeys.hpp>

using namespace std::string_view_literals;
using namespace orchestrator::proto;

TEST_CASE("StoreKeys: a key and its file name", "[orchestrator][proto]")
{
    struct Row
    {
        std::string_view key;
        std::string_view fileName;
    };
    static constexpr auto Rows = std::array {
        Row { .key = "run/pr-1-20261115-120000", .fileName = "run%2Fpr-1-20261115-120000" },
        Row { .key = "..", .fileName = "%2E%2E" },
        Row { .key = ".hidden", .fileName = "%2Ehidden" },
        Row { .key = "a%b", .fileName = "a%25b" },
        Row { .key = "../../etc/passwd", .fileName = "%2E%2E%2F%2E%2E%2Fetc%2Fpasswd" },
        Row { .key = "tab\tnew\nline\x01", .fileName = "tab%09new%0Aline%01" },
        Row { .key = "\xC3\xA4", .fileName = "%C3%A4" },
        Row { .key = "", .fileName = "" },
    };
    for (auto const& row: Rows)
    {
        CAPTURE(row.key);
        CHECK(escapeKey(row.key) == row.fileName);
        CHECK(unescapeKey(row.fileName) == std::string(row.key));
        CHECK_FALSE(escapeKey(row.key).contains('/'));
        CHECK_FALSE(escapeKey(row.key).starts_with('.'));
    }
}

TEST_CASE("StoreKeys: names escapeKey() cannot make are not keys", "[orchestrator][proto]")
{
    static constexpr auto Names = std::array {
        "%tmp"sv, "%2"sv, "%"sv, "%2f"sv, "%41"sv, "a.b"sv, "a b"sv, "%GG"sv,
    };
    for (auto const name: Names)
    {
        CAPTURE(name);
        CHECK(unescapeKey(name) == std::nullopt);
    }
}
