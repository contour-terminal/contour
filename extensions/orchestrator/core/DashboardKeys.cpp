// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/core/DashboardKeys.hpp>

#include <array>
#include <span>
#include <string_view>

using namespace std::string_view_literals;

namespace orchestrator
{

namespace
{
    constexpr auto Keys = std::array {
        DashboardKey { .name = "enter"sv, .character = '\r' },
        DashboardKey { .name = "newline"sv, .character = '\n' },
        DashboardKey { .name = "tab"sv, .character = '\t' },
        DashboardKey { .name = "d"sv, .character = 'd' },
        DashboardKey { .name = "e"sv, .character = 'e' },
        DashboardKey { .name = "b"sv, .character = 'b' },
        DashboardKey { .name = "t"sv, .character = 't' },
        DashboardKey { .name = "x"sv, .character = 'x' },
        DashboardKey { .name = "z"sv, .character = 'z' },
        DashboardKey { .name = "n"sv, .character = 'n' },
        DashboardKey { .name = "/"sv, .character = '/' },
        DashboardKey { .name = "?"sv, .character = '?' },
        DashboardKey { .name = "j"sv, .character = 'j' },
        DashboardKey { .name = "k"sv, .character = 'k' },
        DashboardKey { .name = "R"sv, .character = 'R' },
        DashboardKey { .name = "del"sv, .character = '\0' },
        DashboardKey { .name = "home"sv, .character = '\0' },
        DashboardKey { .name = "end"sv, .character = '\0' },
        DashboardKey { .name = "esc"sv, .character = '\0' },
        DashboardKey { .name = "up"sv, .character = '\0' },
        DashboardKey { .name = "down"sv, .character = '\0' },
        DashboardKey { .name = "left"sv, .character = '\0' },
        DashboardKey { .name = "right"sv, .character = '\0' },
        DashboardKey { .name = "pgup"sv, .character = '\0' },
        DashboardKey { .name = "pgdn"sv, .character = '\0' },
    };
} // namespace

std::span<DashboardKey const> dashboardKeys() noexcept
{
    return Keys;
}

} // namespace orchestrator
