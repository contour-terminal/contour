// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/view/Theme.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <utility>

namespace orchestrator::view
{

namespace
{
    /// One role: the field of core::tui::Theme that carries it, and the style the dashboard gives it.
    struct RoleRow
    {
        Role role = Role::Text;                               ///< The role.
        core::tui::Style core::tui::Theme::* field = nullptr; ///< Where a theme keeps its style.
        core::tui::Style style = {};                          ///< The dashboard's style for it.
    };

    /// The indices of the terminal palette's colours the roles use.
    constexpr auto Red = uint8_t { 1 };
    constexpr auto Green = uint8_t { 2 };
    constexpr auto Yellow = uint8_t { 3 };

    /// @return The terminal's default style with @p change applied.
    template <typename Change>
    [[nodiscard]] constexpr core::tui::Style styled(Change change)
    {
        auto style = core::tui::Style {};
        change(style);
        return style;
    }

    /// @return The terminal's default style in the palette's colour @p index.
    [[nodiscard]] constexpr core::tui::Style coloured(uint8_t index)
    {
        return styled([index](core::tui::Style& style) { style.fg = index; });
    }

    /// Every role, in the order of the enumeration.
    constexpr auto Roles = std::array {
        RoleRow { .role = Role::Text, .field = &core::tui::Theme::textNormal, .style = {} },
        RoleRow { .role = Role::Dim,
                  .field = &core::tui::Theme::textMuted,
                  .style = styled([](core::tui::Style& style) { style.dim = true; }) },
        RoleRow { .role = Role::Warning, .field = &core::tui::Theme::warning, .style = coloured(Yellow) },
        RoleRow { .role = Role::Failing, .field = &core::tui::Theme::error, .style = coloured(Red) },
        RoleRow { .role = Role::Passing, .field = &core::tui::Theme::success, .style = coloured(Green) },
        RoleRow { .role = Role::Selection,
                  .field = &core::tui::Theme::listItemSelected,
                  .style = styled([](core::tui::Style& style) { style.inverse = true; }) },
        RoleRow { .role = Role::Border,
                  .field = &core::tui::Theme::dialogBorder,
                  .style = styled([](core::tui::Style& style) { style.dim = true; }) },
    };

    // styleOf() indexes the table by the role.
    static_assert(
        std::ranges::all_of(std::views::iota(std::size_t { 0 }, Roles.size()), [](std::size_t index) {
            return static_cast<std::size_t>(std::to_underlying(Roles[index].role)) == index;
        }));
} // namespace

core::tui::Theme dashboardTheme()
{
    auto theme = core::tui::Theme {};
    for (auto const& row: Roles)
        theme.*row.field = row.style;
    theme.borderStyle = core::tui::BorderStyle::Rounded;
    return theme;
}

core::tui::Style const& styleOf(core::tui::Theme const& theme, Role role) noexcept
{
    auto const index = static_cast<std::size_t>(std::to_underlying(role));
    return theme.*Roles[index < Roles.size() ? index : 0].field;
}

} // namespace orchestrator::view
