// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/ConfigEnumInfo.hpp>
#include <contour/config/NewDefaultsOffer.hpp>
#include <contour/config/ShellKind.hpp>

#include <core/platform/FileSystem.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace contour::config
{

/// How a window was shown when it closed.
enum class WindowShowState : uint8_t
{
    Normal = 0, ///< At its own frame.
    Maximized,  ///< Maximised; its frame is the one it returns to.
};

namespace detail
{
    /// The show states as the GUI state file spells them. Once shipped, a token is fixed.
    inline constexpr auto WindowShowStateTable = std::array {
        ConfigEnumInfo<WindowShowState> { WindowShowState::Normal, "normal", "Normal" },
        ConfigEnumInfo<WindowShowState> { WindowShowState::Maximized, "maximized", "Maximized" },
    };
} // namespace detail

template <>
constexpr std::span<ConfigEnumInfo<WindowShowState> const> configEnumValues() noexcept
{
    return detail::WindowShowStateTable;
}

/// Where the Settings window was, and what it showed, when it last closed (`settings_window:`).
struct SettingsWindowState
{
    int x = 0;                                      ///< The frame's left edge, in the virtual desktop.
    int y = 0;                                      ///< The frame's top edge.
    int width = 0;                                  ///< The frame's width.
    int height = 0;                                 ///< The frame's height.
    std::string screen {};                          ///< The name of the screen it was on; empty: unknown.
    WindowShowState show = WindowShowState::Normal; ///< Whether it was maximised.
    std::string page {};                            ///< The page it showed; empty: the first one.

    [[nodiscard]] bool operator==(SettingsWindowState const&) const = default;
};

/// What the GUI remembers about the user's answers and its windows, as opposed to what they configured.
///
/// Kept out of `settings.yml` on purpose: that file holds settings, which a reload re-applies, while
/// this holds history, such as an offer the user declined or where a window was, which no setting expresses.
struct GuiState
{
    /// The new-defaults offers the user answered "Keep Mine" to, in the order they did.
    std::vector<NewDefaultsOffer> dismissedOffers {};

    /// The Settings window as it last closed; none before it ever did.
    std::optional<SettingsWindowState> settingsWindow {};

    /// The shells whose "install shell integration" card the user answered "Not Now" to, in the order
    /// they did. A card is never offered again for these; Settings still installs.
    std::vector<ShellKind> dismissedShellIntegrations {};

    [[nodiscard]] bool operator==(GuiState const&) const = default;
};

/// Why the GUI state could not be read or written.
enum class GuiStateError : uint8_t
{
    Unreadable = 0, //!< The file exists but could not be read.
    Malformed,      //!< The file was read but is not the YAML document expected.
    Unwritable,     //!< The file could not be replaced.
};

/// What @p error means, in words for a log line.
/// @param error The failure.
/// @return A short description.
[[nodiscard]] constexpr std::string_view describe(GuiStateError error) noexcept
{
    switch (error)
    {
        case GuiStateError::Unreadable: return "the file could not be read";
        case GuiStateError::Malformed: return "the file is not a GUI state document";
        case GuiStateError::Unwritable: return "the file could not be written";
    }
    return "unknown error";
}

/// Persistence for GuiState.
///
/// An interface per the dependency-injection principle (this is filesystem I/O): the GUI logic that
/// remembers a dismissal takes any store, and the file one is tested on its own.
class GuiStateStore
{
  public:
    virtual ~GuiStateStore() = default;

    /// Reads the state back.
    /// @return The stored state; GuiState {} when nothing has been stored yet.
    [[nodiscard]] virtual std::expected<GuiState, GuiStateError> load() const = 0;

    /// Replaces the stored state with @p state.
    /// @param state The state to store.
    /// @return Nothing on success, or why the write failed.
    [[nodiscard]] virtual std::expected<void, GuiStateError> save(GuiState const& state) = 0;

    /// Changes the stored state: loads it, lets @p change edit it, and saves it. Every writer goes through
    /// here, so what one remembers (a declined offer) is never wiped by another saving its own list. A
    /// state that cannot be loaded is changed from GuiState {}; FileGuiStateStore keeps a copy of such a
    /// file before replacing it.
    /// @param change Edits the loaded state.
    /// @return Nothing on success, or why the write failed.
    [[nodiscard]] std::expected<void, GuiStateError> update(std::function<void(GuiState&)> const& change)
    {
        auto state = load().value_or(GuiState {});
        change(state);
        return save(state);
    }
};

/// The GUI state file's name, beside `contour.yml`.
inline constexpr std::string_view GuiStateFileName = "gui-state.yml";

/// What is appended to GuiStateFileName for the copy of a file that could not be read back.
inline constexpr std::string_view GuiStateBackupSuffix = ".bak";

/// The production GuiStateStore: GuiStateFileName in the configuration directory, read and written through
/// the injected file system, durably (see contour::config::writeFileDurably).
///
/// The file reads `dismissed_new_defaults: [existing_config]`, `dismissed_shell_integration: [zsh]` and
/// `settings_window: { x, y, width, height, screen, show: normal|maximized, page }`. A token no offer or
/// shell answers to is skipped, so a file written by a newer release still loads, and a save keeps those
/// tokens and any other keys. A `settings_window` that is not a map is no state; an unknown `show` word is
/// `normal`.
/// Saving over a file that does not load first copies it to `gui-state.yml.bak` (GuiStateBackupSuffix),
/// and saves nothing when that copy fails.
class FileGuiStateStore final: public GuiStateStore
{
  public:
    /// @param fileSystem The file system the state lives on. Borrowed: it outlives the store.
    /// @param configDir  The directory holding `contour.yml`.
    FileGuiStateStore(core::platform::FileSystem const& fileSystem, std::filesystem::path const& configDir);

    [[nodiscard]] std::expected<GuiState, GuiStateError> load() const override;
    [[nodiscard]] std::expected<void, GuiStateError> save(GuiState const& state) override;

  private:
    core::platform::FileSystem const& _fileSystem;
    std::filesystem::path _file;
};

} // namespace contour::config
