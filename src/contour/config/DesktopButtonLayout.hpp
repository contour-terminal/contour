// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/WindowControlStyle.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace core::platform
{
class FileSystem;
}

namespace contour::config
{

/// Where the desktop puts the window buttons: the ones before the header bar's content and the ones
/// after it.
///
/// Each side lists its buttons in drawing order, leading edge first. A kind appears at most once in the
/// whole layout, and a kind the desktop does not name is not drawn.
struct WindowButtonLayout
{
    std::vector<WindowControlKind> leading {};  ///< The buttons at the leading edge, in drawing order.
    std::vector<WindowControlKind> trailing {}; ///< The buttons at the trailing edge, in drawing order.

    [[nodiscard]] bool operator==(WindowButtonLayout const&) const = default;
};

/// KWin's two button keys as `kwinrc` states them; a missing key takes KWin's default.
struct KWinButtonKeys
{
    std::optional<std::string> left {};  ///< `ButtonsOnLeft`, when set.
    std::optional<std::string> right {}; ///< `ButtonsOnRight`, when set.

    [[nodiscard]] bool operator==(KWinButtonKeys const&) const = default;
};

/// Every place the desktop may state its button layout, each unset where it said nothing.
struct ButtonLayoutSources
{
    /// The portal's `org.gnome.desktop.wm.preferences` `button-layout`, in GTK's syntax.
    std::optional<std::string> gnomeButtonLayout {};
    /// KWin's keys; set only on a KDE session, where an empty value means "KWin's defaults".
    std::optional<KWinButtonKeys> kwin {};
    /// GTK's `gtk-decoration-layout` from `settings.ini`, in GTK's syntax.
    std::optional<std::string> gtkDecorationLayout {};

    [[nodiscard]] bool operator==(ButtonLayoutSources const&) const = default;
};

/// KWin's `ButtonsOnLeft` when `kwinrc` does not set it: the window menu and on-all-desktops.
inline constexpr std::string_view KWinDefaultButtonsOnLeft = "MS";
/// KWin's `ButtonsOnRight` when `kwinrc` does not set it: help, minimize, maximize, close.
inline constexpr std::string_view KWinDefaultButtonsOnRight = "HIAX";

/// The GTK button names that are window buttons. Every other name (`menu`, `appmenu`, `icon`, `spacer`,
/// an unknown one) is skipped: the header bar has its own menu button.
inline constexpr std::array<std::pair<std::string_view, WindowControlKind>, WindowControlCount>
    GtkButtonNames {
        std::pair { std::string_view { "minimize" }, WindowControlKind::Minimize },
        std::pair { std::string_view { "maximize" }, WindowControlKind::Maximize },
        std::pair { std::string_view { "close" }, WindowControlKind::Close },
    };

/// The KWin button letters that are window buttons. Every other letter (`M N S H F B L _`) is skipped.
inline constexpr std::array<std::pair<char, WindowControlKind>, WindowControlCount> KWinButtonLetters {
    std::pair { 'I', WindowControlKind::Minimize },
    std::pair { 'A', WindowControlKind::Maximize },
    std::pair { 'X', WindowControlKind::Close },
};

/// Parses a GTK button layout (`gtk-decoration-layout`, GNOME's `button-layout`) by GTK's own rule.
///
/// The string splits at its first `:` (leading before, trailing after; without one everything is
/// leading), then each side at `,`. Names are trimmed and case-sensitive; a kind already placed is
/// skipped. It never fails: what GTK does not know, it ignores, and so does this.
/// @param layout The layout string, e.g. `appmenu:minimize,maximize,close`.
/// @return The window buttons it names, per side.
[[nodiscard]] WindowButtonLayout parseGtkButtonLayout(std::string_view layout);

/// Parses KWin's two button keys: `left` is the leading side, `right` the trailing one.
/// @param keys The keys as `kwinrc` states them; a missing one takes KWin's default.
/// @return The window buttons they name, per side.
[[nodiscard]] WindowButtonLayout parseKWinButtonLayout(KWinButtonKeys const& keys);

/// Looks a key up in an ini-style text (`kwinrc`, GTK's `settings.ini`).
///
/// Lines starting with `#` or `;` are comments; a key's KDE `[$i]` suffix is ignored; whitespace around
/// the `=` and the value is trimmed; CRLF line ends are tolerated.
/// @param text  The file's contents.
/// @param group The group the key lives in, without its brackets.
/// @param key   The key.
/// @return The key's value in @p group, or nothing when the group or the key is absent.
[[nodiscard]] std::optional<std::string_view> iniValue(std::string_view text,
                                                       std::string_view group,
                                                       std::string_view key);

/// Reads KWin's button keys from `kwinrc`'s `[org.kde.kdecoration2]` group.
/// @param kwinrc The file's contents.
/// @return Each key `kwinrc` sets.
[[nodiscard]] KWinButtonKeys kwinButtonKeysOf(std::string_view kwinrc);

/// What the file sources read the desktop's configuration through.
struct DesktopConfigFiles
{
    core::platform::FileSystem const& fileSystem;    ///< Where the files are read from.
    std::optional<std::filesystem::path> configHome; ///< `$XDG_CONFIG_HOME`; nothing reads no file.
    HostPlatform desktop = HostPlatform::Other; ///< The session, which decides whether KWin's keys apply.
};

/// Reads the button layout sources that live in files: KWin's keys from `<configHome>/kwinrc` on a KDE
/// session (KWin's defaults where the file is missing or unreadable), and GTK's `gtk-decoration-layout`
/// from `gtk-4.0/settings.ini`, else `gtk-3.0/settings.ini`, group `[Settings]`.
/// @param files The file system, the config home and the session.
/// @return The file sources; the portal's GNOME key is never set here.
[[nodiscard]] ButtonLayoutSources fileButtonLayoutSources(DesktopConfigFiles const& files);

/// @return The layout when the desktop states none: minimize, maximize and close on the right, as every
///         Contour window has had them (ruling D3).
[[nodiscard]] WindowButtonLayout fallbackButtonLayout();

/// One place the desktop may state its button layout.
enum class ButtonLayoutSource : uint8_t
{
    GnomeButtonLayout = 0, //!< The portal's `org.gnome.desktop.wm.preferences` `button-layout`.
    KWin,                  //!< KWin's `ButtonsOnLeft` / `ButtonsOnRight`.
    GtkDecorationLayout,   //!< GTK's `gtk-decoration-layout` in `settings.ini`.
};

/// The number of @c ButtonLayoutSource enumerators, derived from the last one.
inline constexpr size_t ButtonLayoutSourceCount =
    static_cast<size_t>(ButtonLayoutSource::GtkDecorationLayout) + 1;

/// The order the sources are asked in, per session, indexed by @c HostPlatform. A KDE session asks KWin
/// first: xdg-desktop-portal merges the Settings of every installed backend, so a Plasma desktop with
/// the GTK backend installed answers GNOME's default `appmenu:close`, which must not outrank the user's
/// KWin buttons.
inline constexpr std::array<std::array<ButtonLayoutSource, ButtonLayoutSourceCount>, HostPlatformCount>
    ButtonLayoutSourceOrders {
        // HostPlatform::Other
        std::array { ButtonLayoutSource::GnomeButtonLayout,
                     ButtonLayoutSource::KWin,
                     ButtonLayoutSource::GtkDecorationLayout },
        // HostPlatform::Windows
        std::array { ButtonLayoutSource::GnomeButtonLayout,
                     ButtonLayoutSource::KWin,
                     ButtonLayoutSource::GtkDecorationLayout },
        // HostPlatform::MacOS
        std::array { ButtonLayoutSource::GnomeButtonLayout,
                     ButtonLayoutSource::KWin,
                     ButtonLayoutSource::GtkDecorationLayout },
        // HostPlatform::KdePlasma
        std::array { ButtonLayoutSource::KWin,
                     ButtonLayoutSource::GnomeButtonLayout,
                     ButtonLayoutSource::GtkDecorationLayout },
    };

/// Resolves the desktop's button layout: the first source, in @p desktop's order, that is set.
/// @param sources What each source said.
/// @param desktop The session, whose row of @c ButtonLayoutSourceOrders gives the order.
/// @return That source's layout, else fallbackButtonLayout().
[[nodiscard]] WindowButtonLayout resolveDesktopButtonLayout(ButtonLayoutSources const& sources,
                                                            HostPlatform desktop);

/// Where @p resolved's controls go.
/// @param resolved A concrete window-control style (config::resolveWindowControlStyle).
/// @param desktop  The desktop's button layout, if it states one.
/// @return For @c Linux, @p desktop, else fallbackButtonLayout(); for any other style, its tokens' order on
///         its side, the other side empty.
[[nodiscard]] WindowButtonLayout windowButtonPlacement(WindowControlStyle resolved,
                                                       std::optional<WindowButtonLayout> const& desktop);

} // namespace contour::config
