// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/Config.h>

#include <vtbackend/Color.h>

#include <expected>
#include <filesystem>
#include <string>

namespace contour
{

/// Persistence for the GUI-managed configuration side files — the write-side counterpart to the
/// loader's side-file merge (see mergeGuiManagedSideFiles in Config.cpp). The settings page creates
/// and edits its own `profiles/<name>.yml` and `colorschemes/<name>.yml`, plus the global
/// `settings.yml`, and never touches the hand-maintained `contour.yml`.
///
/// It is an interface per the project's dependency-injection principle (this is filesystem I/O): the
/// settings-page logic and its tests drive save/delete end to end against an in-memory store, and no
/// caller holds knowledge of paths, temp files, or atomic renames.
class GuiConfigStore
{
  public:
    virtual ~GuiConfigStore() = default;

    /// Writes @p profile to its `profiles/<name>.yml` side file, atomically, creating the `profiles/`
    /// directory if needed.
    /// @param name The profile name (also the file stem).
    /// @param profile The profile to persist.
    /// @return Nothing on success, or a human-readable error describing why the write failed.
    [[nodiscard]] virtual std::expected<void, std::string> saveProfile(
        std::string const& name, config::TerminalProfile const& profile) = 0;

    /// Removes a profile's `profiles/<name>.yml` side file. A file that is already gone is success.
    /// @param name The profile name whose side file to remove.
    /// @return Nothing on success, or a human-readable error.
    [[nodiscard]] virtual std::expected<void, std::string> deleteProfile(std::string const& name) = 0;

    /// Writes @p palette to its `colorschemes/<name>.yml` side file, atomically.
    /// @param name The color scheme name (also the file stem).
    /// @param palette The palette to persist.
    /// @return Nothing on success, or a human-readable error.
    [[nodiscard]] virtual std::expected<void, std::string> saveColorScheme(
        std::string const& name, vtbackend::ColorPalette const& palette) = 0;

    /// Removes a color scheme's `colorschemes/<name>.yml` side file. A file already gone is success.
    /// @param name The color scheme name whose side file to remove.
    /// @return Nothing on success, or a human-readable error.
    [[nodiscard]] virtual std::expected<void, std::string> deleteColorScheme(std::string const& name) = 0;

    /// Writes the GUI-owned global overrides to `settings.yml`, atomically.
    /// @param settings The overrides to persist.
    /// @return Nothing on success, or a human-readable error.
    [[nodiscard]] virtual std::expected<void, std::string> saveGuiSettings(
        config::GuiManagedSettings const& settings) = 0;
};

/// The production GuiConfigStore: side files under the directory holding `contour.yml`, each replaced
/// atomically (write a temp sibling, then rename over the target) so an interrupted or failing write
/// can never leave a truncated file behind — see contour::atomicWriteFile.
class FileGuiConfigStore final: public GuiConfigStore
{
  public:
    /// @param configDir The directory that holds `contour.yml`; every side file lives beside or under
    ///                  it, matching where the loader's side-file merge reads them back from.
    explicit FileGuiConfigStore(std::filesystem::path configDir);

    [[nodiscard]] std::expected<void, std::string> saveProfile(
        std::string const& name, config::TerminalProfile const& profile) override;
    [[nodiscard]] std::expected<void, std::string> deleteProfile(std::string const& name) override;
    [[nodiscard]] std::expected<void, std::string> saveColorScheme(
        std::string const& name, vtbackend::ColorPalette const& palette) override;
    [[nodiscard]] std::expected<void, std::string> deleteColorScheme(std::string const& name) override;
    [[nodiscard]] std::expected<void, std::string> saveGuiSettings(
        config::GuiManagedSettings const& settings) override;

  private:
    std::filesystem::path _configDir;
};

} // namespace contour
