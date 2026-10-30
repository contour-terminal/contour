// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/ConfigEnumInfo.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace contour::config
{

/// Which Option keys act as Alt (Meta) on macOS (`option_as_alt`). The other keeps typing accents like é and
/// ü.
///
/// Zero is `Off`, so a zero-initialised value is the default every edition gives.
enum class OptionAsAlt : uint8_t
{
    Off = 0, //!< Neither Option key is Alt: both compose.
    Left,    //!< The left Option key is Alt.
    Right,   //!< The right Option key is Alt.
    Both,    //!< Either Option key is Alt.
};

/// Which Option keys are held down. The values are bit patterns, one bit a key, so that a set of keys
/// is one value and `Both` is exactly the two bits.
enum class OptionKeySide : uint8_t
{
    None = 0b00,  //!< No Option key is down.
    Left = 0b01,  //!< The left Option key is down.
    Right = 0b10, //!< The right Option key is down.
    Both = 0b11,  //!< Both are.
};

static_assert(static_cast<uint8_t>(OptionKeySide::Both)
                  == (static_cast<uint8_t>(OptionKeySide::Left) | static_cast<uint8_t>(OptionKeySide::Right)),
              "Both is the two keys at once");

/// What a held Option key does to a key press.
enum class OptionKeyRole : uint8_t
{
    Composes = 0, //!< It composes the accent: the key goes on as text.
    Alt,          //!< It is Alt: the key goes on with the Alt modifier.
};

namespace detail
{
    /// The `option_as_alt` spellings. Written into every generated `contour.yml`, so fixed once shipped.
    inline constexpr auto OptionAsAltTable = std::array {
        ConfigEnumInfo<OptionAsAlt> { OptionAsAlt::Off, "off", "Off" },
        ConfigEnumInfo<OptionAsAlt> { OptionAsAlt::Left, "left", "Left" },
        ConfigEnumInfo<OptionAsAlt> { OptionAsAlt::Right, "right", "Right" },
        ConfigEnumInfo<OptionAsAlt> { OptionAsAlt::Both, "both", "Both" },
    };

    /// What `option_as_alt` meant before it named sides: a boolean, as yaml-cpp reads one.
    struct LegacyOptionAsAlt
    {
        std::string_view spelling; ///< The boolean's spelling, in any case.
        OptionAsAlt meaning;       ///< The setting it stands for.
    };

    /// The legacy spellings: true was both keys, false neither. `off` is a token above, so not a row here.
    inline constexpr auto LegacyOptionAsAltTable = std::array {
        LegacyOptionAsAlt { "true", OptionAsAlt::Both }, LegacyOptionAsAlt { "yes", OptionAsAlt::Both },
        LegacyOptionAsAlt { "on", OptionAsAlt::Both },   LegacyOptionAsAlt { "y", OptionAsAlt::Both },
        LegacyOptionAsAlt { "false", OptionAsAlt::Off }, LegacyOptionAsAlt { "no", OptionAsAlt::Off },
        LegacyOptionAsAlt { "n", OptionAsAlt::Off },
    };

    /// The keys each setting makes Alt, one row per setting in enumerator order.
    inline constexpr auto OptionAsAltSides = std::array {
        OptionKeySide::None,  // Off
        OptionKeySide::Left,  // Left
        OptionKeySide::Right, // Right
        OptionKeySide::Both,  // Both
    };

    static_assert(OptionAsAltSides.size() == OptionAsAltTable.size(), "one row of sides per setting");
} // namespace detail

template <>
constexpr std::span<ConfigEnumInfo<OptionAsAlt> const> configEnumValues() noexcept
{
    return detail::OptionAsAltTable;
}

/// The setting a `option_as_alt` value names.
/// @param text The value as written: a token (`off`, `left`, `right`, `both`), or a boolean from before the
///             setting named sides, in any case.
/// @return The setting: `Both` for `true`, `yes`, `on`, `y` and `Off` for `false`, `no`, `n`; nullopt for any
///         other word.
[[nodiscard]] constexpr std::optional<OptionAsAlt> parseOptionAsAlt(std::string_view text) noexcept
{
    if (auto const token = configEnumFromToken<OptionAsAlt>(text))
        return token;
    for (auto const& legacy: detail::LegacyOptionAsAltTable)
        if (detail::equalsIgnoreCase(legacy.spelling, text))
            return legacy.meaning;
    return std::nullopt;
}

/// What a held Option key does under @p setting.
/// @param setting The profile's `option_as_alt`.
/// @param down    Which Option keys are down.
/// @return Alt where a key that is down is one @p setting names, else Composes; nothing down always composes.
[[nodiscard]] constexpr OptionKeyRole optionKeyRole(OptionAsAlt setting, OptionKeySide down) noexcept
{
    auto const named = detail::OptionAsAltSides[static_cast<size_t>(setting)];
    return (static_cast<uint8_t>(named) & static_cast<uint8_t>(down)) != 0 ? OptionKeyRole::Alt
                                                                           : OptionKeyRole::Composes;
}

/// What a key event whose Alt chord is held does under @p setting: the chord is known to be held, so
/// @p setting `Both` is Alt whichever side bits the event carries (a synthesized or forwarded event has
/// none), and a setting naming one side needs that side down.
/// @param setting The profile's `option_as_alt`.
/// @param down    Which Option keys the event says are down.
/// @return Alt or Composes.
[[nodiscard]] constexpr OptionKeyRole optionKeyRoleForHeldChord(OptionAsAlt setting,
                                                                OptionKeySide down) noexcept
{
    return setting == OptionAsAlt::Both ? OptionKeyRole::Alt : optionKeyRole(setting, down);
}

} // namespace contour::config
