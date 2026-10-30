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
    Host,            ///< `hosts/<name>.yml`: one saved remote host.
};

/// One GUI-owned side file, named by what it holds rather than by where it lives.
struct SideFile
{
    SideFileKind kind = SideFileKind::GuiSettings; ///< What the file holds.
    std::string name {}; ///< The profile's, the colour scheme's or the host's name; empty for `settings.yml`.

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

/// @param name The saved host's name.
/// @return The side file of host @p name.
[[nodiscard]] SideFile hostFile(std::string name);

/// Where @p file lives, relative to the directory holding `contour.yml`: `settings.yml`,
/// `profiles/<sideFileName(name)>`, `colorschemes/<sideFileName(name)>` or `hosts/<sideFileName(name)>`.
/// @param file The side file.
/// @return Its path relative to the configuration directory.
[[nodiscard]] std::filesystem::path relativePathOf(SideFile const& file);

/// Whether @p name may name a profile's or a colour scheme's side file: a file name inside `profiles/` or
/// `colorschemes/`, never a path. Refused: an empty name, a `/` or `\\`, a `..`, a leading `.` (a hidden
/// file, and the durable write's temporaries), a `:` (a drive on Windows, or an NTFS stream), and a control
/// character or NUL. The store applies this to every read, write and removal.
/// @param name The name, as the user typed it.
/// @return Nothing, or the reason a page shows.
[[nodiscard]] std::expected<void, std::string> checkSideFileName(std::string_view name);

/// Whether @p name may name a NEW profile or colour scheme: checkSideFileName(), and what Windows will not
/// keep as a file name -- a device name (`CON`, `NUL`, `COM0`-`COM9`, `LPT0`-`LPT9`, their superscript
/// forms, `CONIN$`, `CONOUT$`; also before a dot, and with spaces before it), a trailing dot or space, and
/// any of `<>"|?*`. Refused on every platform, as a configuration directory travels between machines; a file
/// that exists under such a name already stays usable, which is why the store does not apply it.
/// @param name The name, as the user typed it.
/// @return Nothing, or the reason a page shows.
[[nodiscard]] std::expected<void, std::string> checkNewSideFileName(std::string_view name);

/// Whether @p relative, a side file's path relative to the configuration directory, is a file directly
/// inside @p directory: no root name or root directory, a file name (a name that did not convert to a path
/// leaves none), and @p directory as its parent. The store's own
/// guard, beside checkSideFileName(), against a name the platform's path rules read otherwise.
/// @param relative The side file's relative path.
/// @param directory The directory its kind is kept in.
/// @return Whether it stays there.
[[nodiscard]] bool sideFilePathStaysIn(std::filesystem::path const& relative,
                                       std::filesystem::path const& directory);

/// @p file's relative path as a message spells it, in UTF-8 with forward slashes on every platform:
/// `profiles/main.yml`.
/// @param file The side file.
/// @return The path, for a sentence the page shows.
[[nodiscard]] std::string displayPathOf(SideFile const& file);

/// Persistence for the GUI-managed configuration side files — the write-side counterpart to the
/// loader's side-file merge (see mergeGuiManagedSideFiles in Config.cpp). The settings page creates
/// and edits its own `profiles/<name>.yml`, `colorschemes/<name>.yml` and `hosts/<name>.yml`, plus the global
/// `settings.yml`, and never touches the hand-maintained `contour.yml`.
///
/// It speaks bytes, not typed values: the caller serializes (emitProfileYaml, emitColorSchemeYaml,
/// emitHostYaml, emitGuiSettingsYaml) and the store only reads, replaces and removes files. That is what lets
/// a save model compare what is on disk with what it last saw before writing over it.
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
