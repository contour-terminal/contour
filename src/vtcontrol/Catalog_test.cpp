// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cctype>
#include <optional>
#include <set>
#include <string>
#include <tuple>

#include <vtcontrol/Catalog.hpp>
#include <vtcontrol/Json.hpp>

namespace
{
/// Whether `name` is `noun.verb`: lower-case letters, a dot, then letters (camelBack allowed).
bool isMethodName(std::string_view name)
{
    auto const dot = name.find('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 >= name.size())
        return false;
    auto const noun = name.substr(0, dot);
    auto const verb = name.substr(dot + 1);
    return std::ranges::all_of(noun,
                               [](char ch) { return std::islower(static_cast<unsigned char>(ch)) != 0; })
           && std::ranges::all_of(verb,
                                  [](char ch) { return std::isalpha(static_cast<unsigned char>(ch)) != 0; });
}
} // namespace

TEST_CASE("every catalog row is unique and its examples decode", "[vtcontrol][catalog]")
{
    STATIC_REQUIRE(std::tuple_size_v<decltype(vtcontrol::Methods)> == 16);

    auto names = std::set<std::string_view> {};
    std::apply(
        [&](auto const&... row) {
            (
                [&] {
                    using Row = std::remove_cvref_t<decltype(row)>;
                    INFO("method " << row.name);
                    CHECK(isMethodName(row.name));
                    CHECK(names.insert(row.name).second);
                    CHECK_FALSE(row.summary.empty());
                    auto const request = vtcontrol::decodeJson<typename Row::Request>(row.requestExample);
                    CHECK(request.has_value());
                    auto const reply = vtcontrol::decodeJson<typename Row::Reply>(row.responseExample);
                    CHECK(reply.has_value());
                    if (!request)
                        UNSCOPED_INFO(request.error().message);
                    if (!reply)
                        UNSCOPED_INFO(reply.error().message);
                }(),
                ...);
        },
        vtcontrol::Methods);
    CHECK(names.size() == 16);
}

TEST_CASE("permissions and exposures are spelled by their tables", "[vtcontrol][catalog]")
{
    CHECK(vtcontrol::nameOf(vtcontrol::Permission::Screen) == "screen");
    CHECK(vtcontrol::enumFromName<vtcontrol::Permission>("write") == vtcontrol::Permission::Write);
    CHECK_FALSE(vtcontrol::enumFromName<vtcontrol::Permission>("admin"));
}

TEST_CASE("a catalog row's defaults are closed", "[vtcontrol][catalog]")
{
    // A row forgetting its permission or exposure is the strictest, not callable by every extension.
    constexpr auto DefaultRow =
        vtcontrol::Method<vtcontrol::DaemonInfoRequest, vtcontrol::DaemonInfoReply> {};
    STATIC_REQUIRE(DefaultRow.permission == vtcontrol::Permission::Write);
    STATIC_REQUIRE(DefaultRow.exposure == vtcontrol::Exposure::SocketOnly);
}

TEST_CASE("a request-grant rule without a predicate never applies", "[vtcontrol][catalog]")
{
    auto const rule = std::optional { vtcontrol::RequestGrantRule<vtcontrol::CreateTabRequest> {
        .permission = vtcontrol::Permission::Input, .reason = "nothing", .appliesTo = nullptr } };
    CHECK_FALSE(vtcontrol::requestGrantOf(rule, vtcontrol::CreateTabRequest {}));
    CHECK(
        vtcontrol::requestGrantOf(std::optional { vtcontrol::CommandNeedsInput<vtcontrol::CreateTabRequest> },
                                  vtcontrol::CreateTabRequest { .command = { "make" } }));
}
