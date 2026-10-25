// SPDX-License-Identifier: Apache-2.0
#include <contour/config/GuiStateStore.hpp>
#include <contour/test/TempDir.hpp>

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <string_view>

using contour::config::FileGuiStateStore;
using contour::config::GuiState;
using contour::config::GuiStateError;
using contour::config::GuiStateFileName;
using contour::config::NewDefaultsOffer;

namespace
{
/// Writes @p content as the GUI state file in @p dir.
void writeStateFile(std::filesystem::path const& dir, std::string_view content)
{
    auto out = std::ofstream { dir / GuiStateFileName, std::ios::binary };
    out << content;
}
} // namespace

TEST_CASE("GuiStateStore: a missing file is the empty state", "[config][guistate]")
{
    auto const dir = contour::test::TempDir {};
    REQUIRE(dir.isValid());
    auto const store = FileGuiStateStore { dir.path() };
    auto const loaded = store.load();
    REQUIRE(loaded.has_value());
    CHECK(*loaded == GuiState {});
}

TEST_CASE("GuiStateStore: what is saved loads back", "[config][guistate]")
{
    auto const dir = contour::test::TempDir {};
    REQUIRE(dir.isValid());
    auto store = FileGuiStateStore { dir.path() };
    auto const state = GuiState { .dismissedOffers = { NewDefaultsOffer::ExistingConfig,
                                                       NewDefaultsOffer::SettingsOverride } };
    REQUIRE(store.save(state).has_value());
    CHECK(std::filesystem::exists(dir.path() / GuiStateFileName));
    auto const loaded = FileGuiStateStore { dir.path() }.load();
    REQUIRE(loaded.has_value());
    CHECK(*loaded == state);
}

TEST_CASE("GuiStateStore: offers that are not a list are malformed", "[config][guistate]")
{
    auto const dir = contour::test::TempDir {};
    REQUIRE(dir.isValid());
    writeStateFile(dir.path(), "dismissed_new_defaults: 7\n");
    auto const loaded = FileGuiStateStore { dir.path() }.load();
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error() == GuiStateError::Malformed);
}

TEST_CASE("GuiStateStore: an unknown token is skipped, not an error", "[config][guistate]")
{
    auto const dir = contour::test::TempDir {};
    REQUIRE(dir.isValid());
    writeStateFile(dir.path(), "dismissed_new_defaults: [from_the_future, settings_override]\n");
    auto const loaded = FileGuiStateStore { dir.path() }.load();
    REQUIRE(loaded.has_value());
    CHECK(*loaded == GuiState { .dismissedOffers = { NewDefaultsOffer::SettingsOverride } });
}
