// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/view/Theme.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <utility>

using core::tui::operator""_rgb;

namespace orchestrator::view
{

namespace
{
    /// One colour of the design's palette, in both depths.
    struct PaletteColor
    {
        core::tui::RgbColor rgb = {}; ///< In truecolor.
        uint8_t index = 0;            ///< In the 256-colour palette.
    };

    // The design's palette (the spec's "Visual design"): each colour with its 256-colour fallback.
    constexpr auto Selection = PaletteColor { .rgb = 0x20293A_rgb, .index = 236 };
    constexpr auto Muted = PaletteColor { .rgb = 0x8D97A8_rgb, .index = 246 };
    constexpr auto Faint = PaletteColor { .rgb = 0x5B6577_rgb, .index = 242 };
    constexpr auto AccentColor = PaletteColor { .rgb = 0xB4A2FF_rgb, .index = 147 };
    constexpr auto Asking = PaletteColor { .rgb = 0xF4B64E_rgb, .index = 215 };
    constexpr auto ErrorColor = PaletteColor { .rgb = 0xFF8A7A_rgb, .index = 210 };
    constexpr auto WorkingColor = PaletteColor { .rgb = 0x78B0FF_rgb, .index = 111 };
    constexpr auto DoneColor = PaletteColor { .rgb = 0x66D9A8_rgb, .index = 79 };

    /// How a role is drawn, apart from its colours.
    enum class Emphasis : uint8_t
    {
        Plain = 0, ///< As the colours say.
        Italic,    ///< In italics.
    };

    /// One role: the field of core::tui::Theme that carries it, and how the dashboard draws it.
    struct RoleRow
    {
        Role role = Role::Text;                               ///< The role.
        core::tui::Style core::tui::Theme::* field = nullptr; ///< Where a theme keeps its style.
        std::optional<PaletteColor> fg = std::nullopt;        ///< Its foreground; none: the terminal's.
        std::optional<PaletteColor> bg = std::nullopt;        ///< Its background; none: the terminal's.
        Emphasis emphasis = Emphasis::Plain;                  ///< Its emphasis.
    };

    /// Every role, in the order of the enumeration.
    constexpr auto Roles = std::array {
        RoleRow { .role = Role::Text, .field = &core::tui::Theme::textNormal },
        RoleRow { .role = Role::Dim, .field = &core::tui::Theme::textMuted, .fg = Muted },
        RoleRow { .role = Role::Warning, .field = &core::tui::Theme::warning, .fg = Asking },
        RoleRow { .role = Role::Failing, .field = &core::tui::Theme::error, .fg = ErrorColor },
        RoleRow { .role = Role::Passing, .field = &core::tui::Theme::success, .fg = DoneColor },
        RoleRow { .role = Role::Selection, .field = &core::tui::Theme::listItemSelected, .bg = Selection },
        RoleRow { .role = Role::Border, .field = &core::tui::Theme::dialogBorder, .fg = Faint },
        RoleRow { .role = Role::Accent, .field = &core::tui::Theme::textAccent, .fg = AccentColor },
        RoleRow {
            .role = Role::Note, .field = &core::tui::Theme::info, .fg = Muted, .emphasis = Emphasis::Italic },
        RoleRow { .role = Role::Working, .field = &core::tui::Theme::statusKey, .fg = WorkingColor },
        RoleRow { .role = Role::Focus, .field = &core::tui::Theme::buttonFocused, .fg = AccentColor },
    };

    // styleOf() indexes the table by the role.
    static_assert(
        std::ranges::all_of(std::views::iota(std::size_t { 0 }, Roles.size()), [](std::size_t index) {
            return static_cast<std::size_t>(std::to_underlying(Roles[index].role)) == index;
        }));

    /// @return @p color in @p depth; the terminal's own colour when there is none.
    [[nodiscard]] core::tui::Color colorOf(std::optional<PaletteColor> const& color,
                                           ColorDepth depth) noexcept
    {
        if (!color)
            return {};
        if (depth == ColorDepth::TrueColor)
            return color->rgb;
        return color->index;
    }
} // namespace

core::tui::Theme dashboardTheme(ColorDepth depth)
{
    auto theme = core::tui::Theme {};
    for (auto const& row: Roles)
    {
        auto& style = theme.*row.field;
        style = core::tui::Style {};
        style.fg = colorOf(row.fg, depth);
        style.bg = colorOf(row.bg, depth);
        style.italic = row.emphasis == Emphasis::Italic;
    }
    theme.borderStyle = core::tui::BorderStyle::Rounded;
    return theme;
}

core::tui::Style const& styleOf(core::tui::Theme const& theme, Role role) noexcept
{
    auto const index = static_cast<std::size_t>(std::to_underlying(role));
    return theme.*Roles[index < Roles.size() ? index : 0].field;
}

} // namespace orchestrator::view
