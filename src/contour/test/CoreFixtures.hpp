// SPDX-License-Identifier: Apache-2.0
#pragma once

// Fixtures for the Qt-free layers -- the configuration model, the command vocabulary and the CLI.
//
// Separate from GuiTestFixtures.h, which needs a QCoreApplication and a live session manager,
// because these are what contour_test links: a build configured without the GUI frontend has no
// Qt to test against, and that is exactly why it used to have no unit tests at all.

#include <contour/command/CommandCatalog.hpp>
#include <contour/command/CommandHistoryStore.hpp>
#include <contour/config/Config.hpp>
#include <contour/test/TempDir.hpp>

#include <catch2/catch_test_macros.hpp>

#include <expected>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace contour::test
{

/// Loads @p yaml through the PRODUCTION config file loader (writing it to a throwaway temp file
/// first), so a test asserts what a real user's configuration would parse to — including the
/// sibling-layouts merge and every fallback loadConfigFromFile applies.
/// @param yaml The inline configuration document.
/// @return The parsed configuration.
[[nodiscard]] inline contour::config::Config loadConfigFromYaml(std::string_view yaml)
{
    TempDir const dir;
    REQUIRE(dir.isValid());
    auto const path = dir.path() / "contour.yml";
    {
        auto out = std::ofstream(path);
        out << yaml;
    }
    auto config = contour::config::Config {};
    contour::config::loadConfigFromFile(config, path);
    return config;
}

/// An in-memory CommandHistoryStore: the command palette's record -> persist -> reload cycle runs end
/// to end with no filesystem at all. Mirrors InMemoryLayoutStore, including the injectable failures,
/// so a test can drive the "the history file is corrupt" path without corrupting a real one.
class InMemoryCommandHistoryStore final: public contour::command::CommandHistoryStore
{
  public:
    [[nodiscard]] std::expected<std::vector<std::string>, std::string> load(
        std::filesystem::path const& path) const override
    {
        loadedPaths.push_back(path);
        if (loadError)
            return std::unexpected(*loadError);
        return ids;
    }

    [[nodiscard]] std::expected<void, std::string> save(std::filesystem::path const& path,
                                                        std::span<std::string const> newIds) override
    {
        savedPaths.push_back(path);
        if (saveError)
            return std::unexpected(*saveError);
        ids.assign(newIds.begin(), newIds.end());
        return {};
    }

    /// The store's contents, newest first (seed it to model a pre-existing command-history.yml; read
    /// it back to assert what the palette persisted).
    std::vector<std::string> ids;
    /// When set, load() fails with this message (an unreadable/corrupt backing file).
    std::optional<std::string> loadError;
    /// When set, save() fails with this message (permissions, disk full, ...).
    std::optional<std::string> saveError;
    /// The path each load()/save() was asked for, so a test can assert WHERE the history persists.
    mutable std::vector<std::filesystem::path> loadedPaths;
    std::vector<std::filesystem::path> savedPaths;
};

/// A TabTitleProvider over a fixed list of titles, so the command palette's tab source can be driven —
/// and its rows asserted — without a window, an event loop, or a live session behind it.
class StubTabs final: public contour::command::TabTitleProvider
{
  public:
    explicit StubTabs(std::vector<std::string> titles): _titles { std::move(titles) } {}

    [[nodiscard]] std::vector<std::string> tabTitles() const override { return _titles; }

    /// Models tabs opening and closing under a palette that is already showing them.
    void setTitles(std::vector<std::string> titles) { _titles = std::move(titles); }

  private:
    std::vector<std::string> _titles;
};

} // namespace contour::test
