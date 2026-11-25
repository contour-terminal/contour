// SPDX-License-Identifier: Apache-2.0
//
// The text every extension error is shown with.
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <ranges>
#include <string_view>
#include <utility>

#include <vtextension/ExtensionError.hpp>

using namespace std::string_view_literals;
using vtextension::ExtensionError;
using vtextension::ExtensionErrorTexts;

TEST_CASE("ExtensionError.everyErrorHasText", "[vtextension]")
{
    auto const last = std::to_underlying(ExtensionError::HostUnavailable);
    REQUIRE(ExtensionErrorTexts.size() == std::size_t { last } + 1u);
    for (auto const& row: ExtensionErrorTexts)
    {
        INFO("error " << static_cast<int>(std::to_underlying(row.error)));
        CHECK_FALSE(row.text.empty());
        CHECK(vtextension::errorText(row.error) == row.text);
    }
    // Shown to the user, so it names the process, not the engine that ran in it.
    CHECK(vtextension::errorText(ExtensionError::HostUnavailable) == "its host process is unavailable"sv);
}
