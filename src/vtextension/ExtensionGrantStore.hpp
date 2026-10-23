// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/platform/FileSystem.hpp>

#include <compare>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <string_view>

#include <vtextension/CommandPolicy.hpp>

namespace vtextension
{

/// The side file, beside the configuration, that remembers the user's answers to "may this extension run
/// its declared commands?".
inline constexpr auto ExtensionGrantsFileName = std::string_view { "extension-grants.txt" };

/// The side file, beside its configuration, a daemon (`contour daemon`) remembers its users' answers in when
/// it serves the default socket label. Never the GUI's: two processes rewriting one file whole would undo
/// each other's answers, and a daemon's answers come from any attached client and govern commands that run
/// on the daemon's machine -- another authority. @see daemonExtensionGrantsFileName
inline constexpr auto DaemonExtensionGrantsFileName = std::string_view { "extension-grants.daemon.txt" };

/// Which daemon a grants file belongs to: the socket it serves, as its socket label and path name it.
struct DaemonIdentity
{
    std::string_view label;                ///< The socket label it serves.
    std::string_view defaultLabel;         ///< The label a daemon serves when given none.
    std::filesystem::path socketPath;      ///< The socket it serves, canonical.
    std::filesystem::path labelSocketPath; ///< Where a daemon of @ref label serves when no socket is named.
};

/// The grants file of @p daemon: DaemonExtensionGrantsFileName for the default label at its own socket,
/// else one qualified by what tells the daemon apart, so that daemons sharing a configuration directory
/// keep apart -- `extension-grants.daemon.<label>.txt` for another label, and an eight-digit hash of the
/// socket's path (lexically normalized) before `.txt` where a socket other than the label's own is named.
/// @param daemon The daemon. Every character of its label but ASCII letters, digits, `.`, `-` and `_` is
///        spelled `_`, so the name never leaves the directory; an empty label is the default one.
/// @return The file's name.
[[nodiscard]] std::string daemonExtensionGrantsFileName(DaemonIdentity const& daemon);

/// The line a grants file starts with, naming the only format revision there is.
inline constexpr auto ExtensionGrantsHeader = std::string_view { "contour-extension-grants 1" };

/// What one answer is recorded for: an installed extension, and the manifest it was asked with.
struct ExtensionGrantKey
{
    std::string hostId; ///< The host-assigned identity, `<config key>@<canonical module path>`.
    std::string digest; ///< The manifest's digest: a changed manifest is asked about anew.

    [[nodiscard]] auto operator<=>(ExtensionGrantKey const&) const = default;
};

/// Every answer a grants file holds; only answered questions are in it, never a Pending one.
using ExtensionGrants = std::map<ExtensionGrantKey, GrantState>;

/// Why a grants file was not read.
enum class GrantFileError : uint8_t
{
    UnknownFormat, ///< It does not start with ExtensionGrantsHeader: another revision, or not a grants file.
};

/// Reads a grants file.
///
/// The format is lines of text: `#` comments and blank lines, then ExtensionGrantsHeader, then one line per
/// answer, `<granted|denied> TAB <digest> TAB <host id>`. The host id comes last so that it may hold tabs.
/// A line ending in CR is read as if it did not, so a file edited on Windows reads the same. A line that is
/// not an answer is skipped, so one damaged line costs that answer alone.
/// @param text The file's content.
/// @return The answers; UnknownFormat if the header is not the first line that is not a comment.
[[nodiscard]] std::expected<ExtensionGrants, GrantFileError> parseExtensionGrants(std::string_view text);

/// Writes a grants file that parseExtensionGrants() reads back as @p grants.
/// @param grants The answers. One whose host id or digest holds a line break, or whose digest holds a tab,
///        cannot be written, and is left out.
/// @return The file's content.
[[nodiscard]] std::string serializeExtensionGrants(ExtensionGrants const& grants);

/// The production IGrantStore: the answers in memory, kept in a side file beside the
/// configuration (@see ExtensionGrantsFileName).
///
/// Thread-safe: lookup() runs on the extension workers, record() on the GUI thread. The file is read once,
/// at construction, and rewritten whole (to a temporary file, then renamed over it) by every record(). A
/// file that cannot be written leaves the answer remembered for this run only, which is logged.
class ExtensionGrantStore final: public IGrantStore
{
  public:
    /// Reads the answers @p file holds. One that does not exist holds none; one that cannot be read, or is
    /// not a grants file, is logged and holds none either.
    /// @param fileSystem Where @p file is read and written; it must outlive the store.
    /// @param file The grants file.
    ExtensionGrantStore(core::platform::FileSystem const& fileSystem, std::filesystem::path file);

    [[nodiscard]] GrantState lookup(std::string_view hostId, std::string_view digest) const override;

    /// Records the answer, and writes the file. `Pending` forgets the answer recorded before.
    void record(std::string_view hostId, std::string_view digest, GrantState state) override;

  private:
    core::platform::FileSystem const& _fileSystem;
    std::filesystem::path _file;
    mutable std::mutex _mutex;
    ExtensionGrants _grants; ///< Guarded by _mutex.
};

} // namespace vtextension
