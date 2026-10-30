// SPDX-License-Identifier: Apache-2.0
#include <contour/config/DesktopButtonLayout.hpp>

#include <core/Utils.hpp>
#include <core/platform/FileSystem.hpp>

#include <algorithm>
#include <ranges>

namespace contour::config
{

namespace
{
    /// Places @p kind on @p side unless the layout already holds it anywhere.
    void place(WindowButtonLayout& layout, std::vector<WindowControlKind>& side, WindowControlKind kind)
    {
        if (std::ranges::contains(layout.leading, kind) || std::ranges::contains(layout.trailing, kind))
            return;
        side.push_back(kind);
    }

    /// Places every GTK name in the comma-separated @p names on @p side, in order.
    void placeGtkNames(WindowButtonLayout& layout,
                       std::vector<WindowControlKind>& side,
                       std::string_view names)
    {
        for (auto const part: std::views::split(names, ','))
        {
            auto const name = core::trim(std::string_view { part.begin(), part.end() });
            auto const entry = std::ranges::find(
                GtkButtonNames, name, &std::pair<std::string_view, WindowControlKind>::first);
            if (entry != GtkButtonNames.end())
                place(layout, side, entry->second);
        }
    }

    /// Places every KWin letter of @p letters on @p side, in order.
    void placeKWinLetters(WindowButtonLayout& layout,
                          std::vector<WindowControlKind>& side,
                          std::string_view letters)
    {
        for (auto const letter: letters)
        {
            auto const entry =
                std::ranges::find(KWinButtonLetters, letter, &std::pair<char, WindowControlKind>::first);
            if (entry != KWinButtonLetters.end())
                place(layout, side, entry->second);
        }
    }

    /// @return The contents of @p path, or nothing when it cannot be read.
    [[nodiscard]] std::optional<std::string> contentsOf(core::platform::FileSystem const& fileSystem,
                                                        std::filesystem::path const& path)
    {
        auto contents = fileSystem.readFile(path);
        if (!contents)
            return std::nullopt;
        return std::move(*contents);
    }

    /// The GTK `settings.ini` files, the first one that sets the key winning.
    constexpr auto GtkSettingsFiles =
        std::array<std::string_view, 2> { "gtk-4.0/settings.ini", "gtk-3.0/settings.ini" };
} // namespace

WindowButtonLayout parseGtkButtonLayout(std::string_view layout)
{
    auto result = WindowButtonLayout {};
    auto const colon = layout.find(':');
    placeGtkNames(result, result.leading, layout.substr(0, colon));
    if (colon != std::string_view::npos)
        placeGtkNames(result, result.trailing, layout.substr(colon + 1));
    return result;
}

WindowButtonLayout parseKWinButtonLayout(KWinButtonKeys const& keys)
{
    auto result = WindowButtonLayout {};
    placeKWinLetters(result, result.leading, keys.left.value_or(std::string { KWinDefaultButtonsOnLeft }));
    placeKWinLetters(result, result.trailing, keys.right.value_or(std::string { KWinDefaultButtonsOnRight }));
    return result;
}

std::optional<std::string_view> iniValue(std::string_view text, std::string_view group, std::string_view key)
{
    auto inGroup = false;
    for (auto const rawLine: std::views::split(text, '\n'))
    {
        auto const line = core::trim(std::string_view { rawLine.begin(), rawLine.end() });
        if (line.empty() || line.front() == '#' || line.front() == ';')
            continue;
        if (line.front() == '[')
        {
            inGroup = line.size() >= 2 && line.back() == ']' && line.substr(1, line.size() - 2) == group;
            continue;
        }
        if (!inGroup)
            continue;
        auto const equals = line.find('=');
        if (equals == std::string_view::npos)
            continue;
        auto name = core::trim(line.substr(0, equals));
        // KDE's immutability marker, `Key[$i]=value`, belongs to the key's spelling, not its name.
        if (name.ends_with("[$i]"))
            name = core::trim(name.substr(0, name.size() - std::string_view { "[$i]" }.size()));
        if (name == key)
            return core::trim(line.substr(equals + 1));
    }
    return std::nullopt;
}

KWinButtonKeys kwinButtonKeysOf(std::string_view kwinrc)
{
    constexpr auto Group = std::string_view { "org.kde.kdecoration2" };
    auto const toString = [](std::string_view value) {
        return std::string { value };
    };
    return KWinButtonKeys { .left = iniValue(kwinrc, Group, "ButtonsOnLeft").transform(toString),
                            .right = iniValue(kwinrc, Group, "ButtonsOnRight").transform(toString) };
}

ButtonLayoutSources fileButtonLayoutSources(DesktopConfigFiles const& files)
{
    auto sources = ButtonLayoutSources {};
    if (!files.configHome)
        return sources;
    auto const& home = *files.configHome;

    if (files.desktop == HostPlatform::KdePlasma)
        sources.kwin = contentsOf(files.fileSystem, home / "kwinrc")
                           .transform([](std::string const& text) { return kwinButtonKeysOf(text); })
                           .value_or(KWinButtonKeys {});

    for (auto const file: GtkSettingsFiles)
    {
        auto const text = contentsOf(files.fileSystem, home / file);
        if (!text)
            continue;
        if (auto const value = iniValue(*text, "Settings", "gtk-decoration-layout"))
        {
            sources.gtkDecorationLayout = std::string { *value };
            break;
        }
    }
    return sources;
}

WindowButtonLayout fallbackButtonLayout()
{
    return WindowButtonLayout {
        .leading = {},
        .trailing = { WindowControlKind::Minimize, WindowControlKind::Maximize, WindowControlKind::Close },
    };
}

WindowButtonLayout windowButtonPlacement(WindowControlStyle resolved,
                                         std::optional<WindowButtonLayout> const& desktop)
{
    auto const& tokens = windowControlTokens(resolved);
    switch (tokens.placement)
    {
        case ControlPlacement::Desktop: return desktop.value_or(fallbackButtonLayout());
        case ControlPlacement::Own: break;
    }
    auto const order = std::vector<WindowControlKind>(tokens.order.begin(), tokens.order.end());
    switch (tokens.side)
    {
        case WindowControlSide::Leading: return WindowButtonLayout { .leading = order, .trailing = {} };
        case WindowControlSide::Trailing: break;
    }
    return WindowButtonLayout { .leading = {}, .trailing = order };
}

WindowButtonLayout resolveDesktopButtonLayout(ButtonLayoutSources const& sources, HostPlatform desktop)
{
    for (auto const source: ButtonLayoutSourceOrders[static_cast<size_t>(desktop)])
    {
        switch (source)
        {
            case ButtonLayoutSource::GnomeButtonLayout:
                if (sources.gnomeButtonLayout)
                    return parseGtkButtonLayout(*sources.gnomeButtonLayout);
                break;
            case ButtonLayoutSource::KWin:
                if (sources.kwin)
                    return parseKWinButtonLayout(*sources.kwin);
                break;
            case ButtonLayoutSource::GtkDecorationLayout:
                if (sources.gtkDecorationLayout)
                    return parseGtkButtonLayout(*sources.gtkDecorationLayout);
                break;
        }
    }
    return fallbackButtonLayout();
}

} // namespace contour::config
