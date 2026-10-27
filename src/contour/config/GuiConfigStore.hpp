// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/platform/FileSystem.hpp>

#include <compare>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace contour::config
{

/// The kinds of GUI-owned side file the loader merges over `contour.yml`.
enum class SideFileKind : uint8_t
{
    GuiSettings = 0, ///< `settings.yml`: the global overrides and the default profile.
    Profile,         ///< `profiles/<name>.yml`: one profile.
    ColorScheme,     ///< `colorschemes/<name>.yml`: one colour scheme.
};

/// One GUI-owned side file, named by what it holds rather than by where it lives.
struct SideFile
{
    SideFileKind kind = SideFileKind::GuiSettings; ///< What the file holds.
    std::string name {}; ///< The profile's or the colour scheme's name; empty for `settings.yml`.

    [[nodiscard]] auto operator<=>(SideFile const&) const = default;
};

/// @return The `settings.yml` side file.
[[nodiscard]] SideFile guiSettingsFile();

/// @param name The profile's name.
/// @return The side file of profile @p name.
[[nodiscard]] SideFile profileFile(std::string name);

/// @param name The colour scheme's name.
/// @return The side file of colour scheme @p name.
[[nodiscard]] SideFile colorSchemeFile(std::string name);

/// Where @p file lives, relative to the directory holding `contour.yml`: `settings.yml`,
/// `profiles/<sideFileName(name)>` or `colorschemes/<sideFileName(name)>`.
/// @param file The side file.
/// @return Its path relative to the configuration directory.
[[nodiscard]] std::filesystem::path relativePathOf(SideFile const& file);

/// @p file's relative path as a message spells it, in UTF-8 with forward slashes on every platform:
/// `profiles/main.yml`.
/// @param file The side file.
/// @return The path, for a sentence the page shows.
[[nodiscard]] std::string displayPathOf(SideFile const& file);

/// Persistence for the GUI-managed configuration side files — the write-side counterpart to the
/// loader's side-file merge (see mergeGuiManagedSideFiles in Config.cpp). The settings page creates
/// and edits its own `profiles/<name>.yml` and `colorschemes/<name>.yml`, plus the global
/// `settings.yml`, and never touches the hand-maintained `contour.yml`.
///
/// It speaks bytes, not typed values: the caller serializes (emitProfileYaml, emitColorSchemeYaml,
/// emitGuiSettingsYaml) and the store only reads, replaces and removes files. That is what lets a save
/// model compare what is on disk with what it last saw before writing over it.
///
/// It is an interface per the project's dependency-injection principle (this is filesystem I/O): the
/// settings-page logic and its tests drive it against an in-memory store, and no caller holds knowledge
/// of paths, temp files, or atomic renames.
class GuiConfigStore
{
  public:
    virtual ~GuiConfigStore() = default;

    /// Reads @p file.
    /// @param file The side file.
    /// @return Its bytes, nullopt when there is no such file, or why it could not be read.
    [[nodiscard]] virtual std::expected<std::optional<std::string>, std::string> read(
        SideFile const& file) const = 0;

    /// Replaces @p file with @p bytes, durably, creating its directory if needed.
    /// @param file  The side file.
    /// @param bytes Its new content.
    /// @return Nothing on success, or why the write failed; the old file is then intact.
    [[nodiscard]] virtual std::expected<void, std::string> write(SideFile const& file,
                                                                 std::string_view bytes) = 0;

    /// Removes @p file. A file that is already gone is success.
    /// @param file The side file.
    /// @return Nothing on success, or why it could not be removed.
    [[nodiscard]] virtual std::expected<void, std::string> remove(SideFile const& file) = 0;

    /// Lists the side files of @p kind that exist, name-sorted.
    /// @param kind The kind of side file.
    /// @return The files, none when their directory is absent, or why it could not be listed.
    [[nodiscard]] virtual std::expected<std::vector<SideFile>, std::string> list(SideFileKind kind) const = 0;
};

/// The production GuiConfigStore: side files under the directory holding `contour.yml`, each replaced
/// durably through the injected file system — see writeFileDurably — so an interrupted or failing
/// write can never leave a truncated file behind.
class FileGuiConfigStore final: public GuiConfigStore
{
  public:
    /// @param fileSystem The file system the side files live on. Borrowed: it outlives the store.
    /// @param configDir  The directory that holds `contour.yml`; every side file lives beside or under
    ///                   it, matching where the loader's side-file merge reads them back from.
    FileGuiConfigStore(core::platform::FileSystem const& fileSystem, std::filesystem::path configDir);

    [[nodiscard]] std::expected<std::optional<std::string>, std::string> read(
        SideFile const& file) const override;
    [[nodiscard]] std::expected<void, std::string> write(SideFile const& file,
                                                         std::string_view bytes) override;
    [[nodiscard]] std::expected<void, std::string> remove(SideFile const& file) override;
    [[nodiscard]] std::expected<std::vector<SideFile>, std::string> list(SideFileKind kind) const override;

  private:
    core::platform::FileSystem const& _fileSystem;
    std::filesystem::path _configDir;
};

} // namespace contour::config
