// SPDX-License-Identifier: Apache-2.0
//
// Fixtures shared by the Config_test*.cpp files: the YAML writers and loaders every case goes through.

#pragma once

#include <contour/config/Config.hpp>
#include <contour/config/ConfigEnum.hpp>

#include <vtbackend/core/ColorPalette.hpp>
#include <vtbackend/input/InputGenerator.hpp>

#include <QtCore/QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <variant>

namespace contour::test::configSupport
{

using namespace std::string_view_literals;

/// Writes @p yaml to a fresh file inside @p dir and returns its path.
[[nodiscard]] inline std::filesystem::path writeConfig(QTemporaryDir& dir, std::string_view yaml)
{
    auto const path = std::filesystem::path(dir.path().toStdU16String()) / "contour.yml";
    {
        auto out = std::ofstream(path);
        out << yaml;
    }
    return path;
}

/// Loads a config from inline YAML through the production file loader.
[[nodiscard]] inline contour::config::Config loadFromYaml(QTemporaryDir& dir, std::string_view yaml)
{
    auto config = contour::config::Config {};
    contour::config::loadConfigFromFile(config, writeConfig(dir, yaml));
    return config;
}

/// Loads a config from inline YAML as the keyboard platform @p kind reads it: `Primary` resolves for it, and
/// the default bindings are its own.
[[nodiscard]] inline contour::config::Config loadFromYaml(QTemporaryDir& dir,
                                                          std::string_view yaml,
                                                          contour::config::KeyboardPlatformKind kind)
{
    auto config = contour::config::Config { .keyboardPlatformKind = kind };
    contour::config::loadConfigFromFile(config, writeConfig(dir, yaml));
    return config;
}

/// The main profile's color scheme, which @p config must give as a single scheme rather than a dark/light
/// pair.
[[nodiscard]] inline vtbackend::ColorPalette schemeOf(contour::config::Config const& config)
{
    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    REQUIRE(std::holds_alternative<contour::config::SimpleColorConfig>(profile->colors.value()));
    return std::get<contour::config::SimpleColorConfig>(profile->colors.value()).colors;
}

/// Writes @p content to a GUI side file at @p relativePath under @p dir (the config directory),
/// creating intermediate directories (e.g. `profiles/`, `colorschemes/`) as needed.
inline void writeSideFile(QTemporaryDir& dir,
                          std::filesystem::path const& relativePath,
                          std::string_view content)
{
    auto const path = std::filesystem::path(dir.path().toStdU16String()) / relativePath;
    std::filesystem::create_directories(path.parent_path());
    auto out = std::ofstream(path);
    out << content;
}

/// A Classic configuration, for the tests that edit the "default" colour scheme: a Classic profile
/// names that scheme, whereas a Native2026 one names contour-dark/-light.
[[nodiscard]] inline contour::config::Config classicConfig()
{
    return contour::config::Config { .defaultsEdition = contour::config::DefaultsEdition::Classic };
}

/// Every enumerator of @p Enum has a row in its table, and no other row exists.
///
/// The table is the single source the YAML reader, the config writer, the settings page and any menu
/// offering these values all read from. An enumerator missing a row could therefore not be written,
/// read back, or shown -- so pin that every one of them has a row with a label, and that a row's token
/// survives the round trip the config file depends on. Walking the enumerators rather than the rows
/// names the enumerator that has none.
/// @param enumerators Every enumerator of @p Enum.
template <typename Enum, size_t Count>
void checkEnumerators(std::array<Enum, Count> const& enumerators)
{
    using namespace contour::config;
    CHECK(configEnumValues<Enum>().size() == Count);
    for (auto const value: enumerators)
    {
        auto const token = configEnumToken(value);
        INFO("enumerator: " << static_cast<int>(value) << ", token: " << token);
        CHECK(!token.empty());
        CHECK(!configEnumLabel(value).empty());
        CHECK(configEnumFromToken<Enum>(token) == value);
    }
}

/// The mapping bound to @p key in @p config, or nullptr.
[[nodiscard]] inline contour::config::KeyInputMapping const* keyMappingFor(
    contour::config::Config const& config, vtbackend::Key key)
{
    auto const& mappings = config.inputMappings.value().keyMappings;
    auto const i = std::ranges::find(mappings, key, &contour::config::KeyInputMapping::input);
    return i != mappings.end() ? &*i : nullptr;
}

} // namespace contour::test::configSupport
