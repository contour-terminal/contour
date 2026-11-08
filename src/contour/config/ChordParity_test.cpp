// SPDX-License-Identifier: Apache-2.0
//
// Every application command that has a default keyboard chord on Linux and Windows has one on macOS, and
// the other way round, so that moving between machines never loses a command. A command bound on one side
// only is listed below with the reason, and the list is checked in both directions: a gap that is not listed
// fails, and so does a listed exemption that is no longer needed.

#include <contour/config/Actions.hpp>
#include <contour/config/Config.hpp>
#include <contour/config/DefaultBindings.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <iterator>
#include <set>
#include <string>
#include <string_view>

using namespace contour;
using namespace contour::config;

namespace
{

/// A command deliberately bound on one platform only.
struct Exemption
{
    std::string_view action {}; ///< The action's name.
    KeyboardPlatformKind boundOn =
        KeyboardPlatformKind::Xkb; ///< The one platform that binds it (Xkb stands for Linux and Windows).
    std::string_view reason {};    ///< Why the other platform does not.
};

/// The commands bound on one platform only (@see
/// docs/superpowers/specs/2026-11-08-platform-chord-parity-design.md).
constexpr auto Exemptions = std::array {
    Exemption { .action = "Quit",
                .boundOn = KeyboardPlatformKind::Xkb,
                .reason = "the macOS menu bar owns Command+Q" },
    Exemption { .action = "SpeakSelection",
                .boundOn = KeyboardPlatformKind::Xkb,
                .reason = "macOS has a system-wide Speak Selection (Option+Esc)" },
    Exemption { .action = "CloseTab",
                .boundOn = KeyboardPlatformKind::Mac,
                .reason = "on the desktop Primary+Shift+W collapses to ClosePane's Ctrl+Shift+W" },
    Exemption { .action = "FocusToolbar",
                .boundOn = KeyboardPlatformKind::Mac,
                .reason = "AppKit's Control+F5; the desktop has no toolbar chord" },
    Exemption { .action = "SearchSelection",
                .boundOn = KeyboardPlatformKind::Mac,
                .reason = "a macOS find-pasteboard convention; Ctrl+Shift+E is SplitVertical" },
    Exemption { .action = "SendChars",
                .boundOn = KeyboardPlatformKind::Mac,
                .reason =
                    "the Mac word and line motions; desktop shells handle Ctrl+Left/Right, Home and End" },
};

/// The names of the commands with a keyboard chord on @p kind: the first action of every key and character
/// row, in both editions' defaults and in the fallbacks.
///
/// The union over the editions, not one set per edition: the only edition-scoped keyboard rows are the
/// Native2026 chrome-focus chords, which would otherwise make FocusToolbar's exemption stale in Classic.
/// @param kind The keyboard platform.
/// @return The action names.
[[nodiscard]] std::set<std::string_view> keyboardActions(KeyboardPlatformKind kind)
{
    auto result = std::set<std::string_view> {};
    auto const take = [&](auto const& binding) {
        if (!binding.empty())
            result.insert(actions::name(binding.front()));
    };
    for (auto const& [edition, token, label]: configEnumValues<DefaultsEdition>())
    {
        auto const mappings = makeDefaultInputMappings(edition, kind);
        for (auto const& row: mappings.keyMappings)
            take(row.binding);
        for (auto const& row: mappings.charMappings)
            take(row.binding);
    }
    for (auto const& row: builtinFallbackKeyMappings(kind))
        take(row.mapping.binding);
    for (auto const& row: builtinFallbackCharMappings(kind))
        take(row.mapping.binding);
    return result;
}

/// The names in @p from that @p other lacks.
[[nodiscard]] std::set<std::string_view> missingFrom(std::set<std::string_view> const& from,
                                                     std::set<std::string_view> const& other)
{
    auto result = std::set<std::string_view> {};
    std::ranges::set_difference(from, other, std::inserter(result, result.end()));
    return result;
}

/// The exempted names bound on @p kind only.
[[nodiscard]] std::set<std::string_view> exemptOn(KeyboardPlatformKind kind)
{
    auto result = std::set<std::string_view> {};
    for (auto const& exemption: Exemptions)
        if (exemption.boundOn == kind)
            result.insert(exemption.action);
    return result;
}

/// One line per name, with its exemption's reason when it has one.
[[nodiscard]] std::string describe(std::set<std::string_view> const& names)
{
    auto text = std::string {};
    for (auto const name: names)
    {
        auto const exemption = std::ranges::find(Exemptions, name, &Exemption::action);
        text += std::string { name };
        if (exemption != Exemptions.end())
            text += std::string { " (exempt: " } + std::string { exemption->reason } + ")";
        text += '\n';
    }
    return text;
}

} // namespace

TEST_CASE("every keyboard command is bound on both Linux/Windows and macOS, or exempt with a reason",
          "[bindings][parity]")
{
    auto const desktop = keyboardActions(KeyboardPlatformKind::Xkb);
    auto const mac = keyboardActions(KeyboardPlatformKind::Mac);

    auto const desktopOnly = missingFrom(desktop, mac);
    auto const macOnly = missingFrom(mac, desktop);
    INFO("bound on Linux and Windows only:\n" << describe(desktopOnly));
    INFO("bound on macOS only:\n" << describe(macOnly));
    CHECK(desktopOnly == exemptOn(KeyboardPlatformKind::Xkb));
    CHECK(macOnly == exemptOn(KeyboardPlatformKind::Mac));
}

TEST_CASE("Windows binds the same keyboard commands as Linux", "[bindings][parity]")
{
    CHECK(keyboardActions(KeyboardPlatformKind::Win32) == keyboardActions(KeyboardPlatformKind::Xkb));
}
