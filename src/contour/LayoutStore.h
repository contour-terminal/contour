// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/Config.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>

namespace contour
{

/// Named layouts, keyed by layout name — the unit of layout persistence.
using LayoutMap = std::unordered_map<std::string, config::Layout>;

/// Why a layout could not be saved. Each value names a distinct, user-actionable cause, so the
/// caller can report it rather than just "it failed".
enum class LayoutSaveError : std::uint8_t
{
    UnknownWindow,   ///< The window to serialize does not exist (it closed mid-action).
    EmptyName,       ///< The SaveLayout action carried no layout name.
    StoreUnreadable, ///< The existing store could not be read back, so rewriting it would destroy it.
    WriteFailed,     ///< The store could not be written (permissions, disk full, I/O error).
};

/// A human-readable explanation of @p error, for logs and user-facing notices.
/// @param error The failure to describe.
/// @return A short sentence naming the cause.
[[nodiscard]] constexpr std::string_view describe(LayoutSaveError error) noexcept
{
    switch (error)
    {
        case LayoutSaveError::UnknownWindow: return "the window no longer exists";
        case LayoutSaveError::EmptyName: return "no layout name was given";
        case LayoutSaveError::StoreUnreadable:
            return "the existing layouts file could not be read; fix or remove it";
        case LayoutSaveError::WriteFailed: return "the layouts file could not be written";
    }
    return "unknown error";
}

/// Persistence for named layouts: the boundary between the session manager's in-memory layout
/// model and wherever those layouts actually live.
///
/// It is an interface per the project's dependency-injection principle (this is filesystem I/O),
/// so tests drive SaveLayout end-to-end against an in-memory store, and the manager itself holds
/// no knowledge of files, temp names, or atomic renames.
class LayoutStore
{
  public:
    virtual ~LayoutStore() = default;

    /// Reads back every layout the store currently holds.
    /// @param path Where the store persists (the sibling `layouts.yml` of the loaded config file).
    /// @return The stored layouts (empty when nothing has been saved yet), or a human-readable
    ///         error when the store exists but cannot be read. Callers MUST treat an error as
    ///         "do not overwrite": rewriting an unreadable store would destroy what it still holds.
    [[nodiscard]] virtual std::expected<LayoutMap, std::string> load(
        std::filesystem::path const& path) const = 0;

    /// Replaces the store's entire contents with @p layouts, atomically.
    /// @param path    Where the store persists.
    /// @param layouts The layouts to persist.
    /// @return Nothing on success, or a human-readable error describing why the write failed.
    [[nodiscard]] virtual std::expected<void, std::string> save(std::filesystem::path const& path,
                                                                LayoutMap const& layouts) = 0;
};

/// The production LayoutStore: a YAML file, replaced atomically (write a temp sibling, then
/// rename over the target) so an interrupted or failing write can never leave a truncated
/// `layouts.yml` behind — losing a truncated store means losing every layout the user ever saved.
class FileLayoutStore final: public LayoutStore
{
  public:
    [[nodiscard]] std::expected<LayoutMap, std::string> load(
        std::filesystem::path const& path) const override;

    [[nodiscard]] std::expected<void, std::string> save(std::filesystem::path const& path,
                                                        LayoutMap const& layouts) override;
};

} // namespace contour
