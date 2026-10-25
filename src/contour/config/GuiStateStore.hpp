// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/NewDefaultsOffer.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string_view>
#include <vector>

namespace contour::config
{

/// What the GUI remembers about the user's answers, as opposed to what they configured.
///
/// Kept out of `settings.yml` on purpose: that file holds settings, which a reload re-applies, while
/// this holds history, such as an offer the user declined, which no setting expresses.
struct GuiState
{
    /// The new-defaults offers the user answered "Keep Mine" to, in the order they did.
    std::vector<NewDefaultsOffer> dismissedOffers {};

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
};

/// The GUI state file's name, beside `contour.yml`.
inline constexpr std::string_view GuiStateFileName = "gui-state.yml";

/// What is appended to GuiStateFileName for the copy of a file that could not be read back.
inline constexpr std::string_view GuiStateBackupSuffix = ".bak";

/// The production GuiStateStore: GuiStateFileName in the configuration directory, written atomically
/// (see contour::config::atomicWriteFile) as FileGuiConfigStore writes `settings.yml`.
///
/// The file reads `dismissed_new_defaults: [existing_config]`. A token no offer answers to is skipped,
/// so a file written by a newer release still loads, and a save keeps those tokens and any other keys.
/// Saving over a file that does not load first copies it to `gui-state.yml.bak` (GuiStateBackupSuffix),
/// and saves nothing when that copy fails.
class FileGuiStateStore final: public GuiStateStore
{
  public:
    /// @param configDir The directory holding `contour.yml`.
    explicit FileGuiStateStore(std::filesystem::path configDir);

    [[nodiscard]] std::expected<GuiState, GuiStateError> load() const override;
    [[nodiscard]] std::expected<void, GuiStateError> save(GuiState const& state) override;

  private:
    std::filesystem::path _file;
};

} // namespace contour::config
