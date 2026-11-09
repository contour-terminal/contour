// SPDX-License-Identifier: Apache-2.0
#include <contour/cli/ClientAttach.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

using contour::cli::launchIntent;
using vthost::proto::AttachIntentKind;

TEST_CASE("launchIntent: a launch follows the left workspaces, --workspace exactly one", "[cli][client]")
{
    CHECK(launchIntent(std::nullopt).kind == AttachIntentKind::Launch);
    CHECK(launchIntent(std::nullopt).workspaces.empty());
    CHECK(launchIntent(uint64_t { 7 }).kind == AttachIntentKind::Workspaces);
    CHECK(launchIntent(uint64_t { 7 }).workspaces == std::vector<uint64_t> { 7 });
}

TEST_CASE("HostExclusions: --workspace may be given beside --host", "[cli][client]")
{
    CHECK(std::ranges::none_of(contour::cli::HostExclusions,
                               [](auto const& option) { return option.name == "workspace"; }));
    // Where and how to connect stays the host entry's to say.
    for (auto const name: { "connect-tcp", "socket", "tmux", "token", "token-file", "tls-ca" })
        CHECK(std::ranges::any_of(contour::cli::HostExclusions,
                                  [name](auto const& option) { return option.name == name; }));
}
