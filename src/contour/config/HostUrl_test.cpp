// SPDX-License-Identifier: Apache-2.0
#include <contour/config/HostUrl.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string_view>
#include <utility>

using contour::config::describe;
using contour::config::HostScheme;
using contour::config::HostUrlError;
using contour::config::parseHostUrl;

namespace
{

struct AcceptedRow
{
    std::string_view input;
    HostScheme scheme;
    std::string_view user;
    std::string_view host;
    std::optional<std::uint16_t> port;
};

constexpr auto AcceptedRows = std::array {
    AcceptedRow { "ssh://homepc", HostScheme::Ssh, "", "homepc", std::nullopt },
    AcceptedRow { "ssh://me@homepc", HostScheme::Ssh, "me", "homepc", std::nullopt },
    AcceptedRow { "ssh://me@homepc:2222", HostScheme::Ssh, "me", "homepc", 2222 },
    AcceptedRow { "ssh://me@[fe80::1]:22", HostScheme::Ssh, "me", "fe80::1", 22 },
    AcceptedRow { "tls://studio.tailnet.ts.net:7297", HostScheme::Tls, "", "studio.tailnet.ts.net", 7297 },
    AcceptedRow { "SSH://homepc", HostScheme::Ssh, "", "homepc", std::nullopt },
    AcceptedRow { "ssh://first.last@homepc", HostScheme::Ssh, "first.last", "homepc", std::nullopt },
    AcceptedRow { "ssh://me@[fe80::1%eth0]", HostScheme::Ssh, "me", "fe80::1%eth0", std::nullopt },
};

struct RefusedRow
{
    std::string_view input;
    HostUrlError error;
};

constexpr auto RefusedRows = std::array {
    RefusedRow { "homepc", HostUrlError::MissingScheme },
    RefusedRow { "http://x", HostUrlError::UnknownScheme },
    RefusedRow { "ssh://", HostUrlError::MissingHost },
    RefusedRow { "ssh://me@", HostUrlError::MissingHost },
    RefusedRow { "ssh://-oProxyCommand=x", HostUrlError::OptionLikeHost },
    RefusedRow { "ssh://-l@homepc", HostUrlError::OptionLikeUser },
    RefusedRow { "ssh://ho st", HostUrlError::UnsafeHost },
    RefusedRow { "ssh://homepc;id", HostUrlError::UnsafeHost },
    RefusedRow { "ssh://home\npc", HostUrlError::UnsafeHost },
    RefusedRow { "ssh://$(x)@homepc", HostUrlError::UnsafeUser },
    RefusedRow { "ssh://me`id`@homepc", HostUrlError::UnsafeUser },
    RefusedRow { "ssh://homepc:0", HostUrlError::BadPort },
    RefusedRow { "ssh://homepc:65536", HostUrlError::BadPort },
    RefusedRow { "ssh://homepc:22x", HostUrlError::BadPort },
    RefusedRow { "tls://studio", HostUrlError::PortRequired },
    RefusedRow { "ssh://homepc/path", HostUrlError::UnexpectedPath },
    RefusedRow { "tls://me@studio:7297", HostUrlError::UserNotAllowed },
};

} // namespace

TEST_CASE("parseHostUrl accepts", "[config][hosts]")
{
    for (auto const& row: AcceptedRows)
    {
        INFO(row.input);
        auto const url = parseHostUrl(row.input);
        REQUIRE(url.has_value());
        CHECK(url->scheme == row.scheme);
        CHECK(url->user == row.user);
        CHECK(url->host == row.host);
        CHECK(url->port == row.port);
    }
}

TEST_CASE("parseHostUrl refuses", "[config][hosts]")
{
    for (auto const& row: RefusedRows)
    {
        INFO(row.input);
        auto const url = parseHostUrl(row.input);
        REQUIRE_FALSE(url.has_value());
        CHECK(url.error() == row.error);
    }
}

TEST_CASE("toString spells a host URL canonically", "[config][hosts]")
{
    CHECK(toString(*parseHostUrl("ssh://me@[fe80::1]:22")) == "ssh://me@[fe80::1]:22");
    CHECK(toString(*parseHostUrl("SSH://homepc")) == "ssh://homepc");
    CHECK(toString(*parseHostUrl("tls://studio:7297")) == "tls://studio:7297");
}

TEST_CASE("describe says something for every host URL error", "[config][hosts]")
{
    auto const count = std::to_underlying(HostUrlError::UserNotAllowed) + 1;
    for (auto const value: std::views::iota(0, count))
        CHECK_FALSE(describe(static_cast<HostUrlError>(value)).empty());
}
