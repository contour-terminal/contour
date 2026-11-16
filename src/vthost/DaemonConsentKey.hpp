// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The daemon's consent key: the secret a Contour window proves it read before the daemon asks it -- and lets
/// it answer -- a daemon extension's grant (proto::Capability::DaemonExtensionConsent).
///
/// A daemon makes a fresh key once it holds its socket, in a private file of its own instance (0600, in a
/// 0700 directory opened without following a link), and never puts that file's path in any pane's
/// environment. A window reads the file of the instance the daemon's hello names, and answers the hello's
/// fresh nonce with an HMAC bound to that instance and the socket; the key never crosses the socket. What it
/// keeps out: an extension, a control verb, and any process that merely inherits a pane's environment. What
/// it does not: a process of the same user that reads the key file on purpose can pass for a window, as it
/// could fake key presses -- the user is the trust boundary.

#include <core/Random.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vthost
{

/// How many bytes a consent key, a nonce and a proof each hold.
inline constexpr std::size_t ConsentSecretBytes = 32;

/// The daemon's consent key.
using ConsentKey = std::array<std::byte, ConsentSecretBytes>;

/// What the daemon's hello asks a window to prove the key over: fresh per connection.
using ConsentNonce = std::array<std::byte, ConsentSecretBytes>;

/// A window's answer (consentProofOf()).
using ConsentProofMac = std::array<std::byte, ConsentSecretBytes>;

/// What a proof is bound to besides the nonce: which daemon run, at which socket.
struct ConsentContext
{
    uint64_t daemonInstance = 0; ///< The run the hello names (proto::ServerHello::daemonInstance).
    std::string socket;          ///< The daemon's socket, canonical, as UTF-8.
};

/// Makes a window's proof for the run its daemon's hello names, over that hello's nonce: reads the key of
/// that run as the hello arrives, so a daemon replaced since the connect is proved with its own key. Nothing
/// when the key cannot be read, and the window is then asked nothing.
using ConsentProver =
    std::function<std::optional<ConsentProofMac>(uint64_t daemonInstance, ConsentNonce const& nonce)>;

/// Whether a proof holds.
enum class ConsentProofCheck : uint8_t
{
    Refused = 0, ///< It is not the key's over this context and nonce.
    Proven,      ///< It is: the peer read the key.
};

/// A key file kept for the run that made it: while this lives, no other run's clean-up removes the file
/// (createDaemonConsentKey()). Released as it is destroyed, and never inherited by a child.
class ConsentKeyHold
{
  public:
    ConsentKeyHold() = default;
    ConsentKeyHold(ConsentKeyHold const&) = delete;
    ConsentKeyHold(ConsentKeyHold&&) = delete;
    ConsentKeyHold& operator=(ConsentKeyHold const&) = delete;
    ConsentKeyHold& operator=(ConsentKeyHold&&) = delete;
    virtual ~ConsentKeyHold() = default;
};

/// What a clean-up did with a key file (IConsentKeyFiles::removeUnlessHeld()).
enum class KeyFileRemoval : uint8_t
{
    Held = 0, ///< A live run holds it: it is kept.
    Removed,  ///< No run held it: it is gone (or was already).
};

/// The files of the consent directory, each reached through the directory held open: a directory that is a
/// link, or not the user's alone, is refused before anything in it is touched.
class IConsentKeyFiles
{
  public:
    IConsentKeyFiles() = default;
    IConsentKeyFiles(IConsentKeyFiles const&) = delete;
    IConsentKeyFiles(IConsentKeyFiles&&) = delete;
    IConsentKeyFiles& operator=(IConsentKeyFiles const&) = delete;
    IConsentKeyFiles& operator=(IConsentKeyFiles&&) = delete;
    virtual ~IConsentKeyFiles() = default;

    /// Puts @p content in @p directory as @p name, whole, and holds the file: the directory made (0700) if
    /// missing; the file made 0600 under a temporary name while empty, held, written, synced, and renamed
    /// over @p name.
    /// @return The hold, which keeps every removeUnlessHeld() off the file -- the temporary one included --
    ///         until it is destroyed.
    [[nodiscard]] virtual std::expected<std::unique_ptr<ConsentKeyHold>, std::string> keep(
        std::filesystem::path const& directory, std::string_view name, std::string_view content) const = 0;

    /// @return The content of @p name in @p directory: a regular file, the user's alone, read through no
    /// link.
    [[nodiscard]] virtual std::expected<std::string, std::string> read(std::filesystem::path const& directory,
                                                                       std::string_view name) const = 0;

    /// @return The names of the entries of @p directory; none for one that does not exist.
    [[nodiscard]] virtual std::expected<std::vector<std::string>, std::string> list(
        std::filesystem::path const& directory) const = 0;

    /// Removes @p name from @p directory unless a live run holds it (keep()); one already gone is removed.
    /// @return Whether it was removed or kept as held, or why neither could be told.
    [[nodiscard]] virtual std::expected<KeyFileRemoval, std::string> removeUnlessHeld(
        std::filesystem::path const& directory, std::string_view name) const = 0;
};

/// @return The OS's: `openat`, `fchmod` and `renameat` beneath a directory opened `O_DIRECTORY|O_NOFOLLOW`
///         and checked by `fstat`, a hold that is an exclusive `flock` on POSIX; a protected DACL, a hold
///         that is a handle sharing no delete on Windows.
[[nodiscard]] IConsentKeyFiles const& nativeConsentKeyFiles();

/// @param stateDirectory The daemon's state directory (`daemonStateDirectory(...)`, StatePath.hpp).
/// @return `<stateDirectory>/consent`.
[[nodiscard]] std::filesystem::path daemonConsentKeyDirectory(std::filesystem::path const& stateDirectory);

/// @param socket The daemon's socket, canonical: what both the daemon and a window on its machine know of it.
/// @param daemonInstance The daemon's run.
/// @return `<16 hex digits of the FNV-1a 64 of the socket's path>-<16 hex digits of the run>.key`.
[[nodiscard]] std::string daemonConsentKeyName(std::filesystem::path const& socket, uint64_t daemonInstance);

/// A run's consent key, and the hold on its file: the daemon keeps both for as long as it serves.
struct HeldConsentKey
{
    ConsentKey key;                       ///< The key.
    std::unique_ptr<ConsentKeyHold> hold; ///< Keeps every other run's clean-up off the key's file.
};

/// Makes a fresh key for this run of the daemon at @p socket, keeps it in @p directory and holds its file,
/// then removes the key files earlier runs at the same socket left. Only a file no live run holds is
/// removed: should two daemons ever hold the same socket at once, neither removes the other's key.
/// @param files The consent directory's files.
/// @param random Where the key's bytes come from: a CSPRNG.
/// @param directory daemonConsentKeyDirectory().
/// @param socket The daemon's socket, canonical.
/// @param daemonInstance This run.
/// @return The key and its hold, or why it cannot be kept; the daemon then asks no window.
[[nodiscard]] std::expected<HeldConsentKey, std::string> createDaemonConsentKey(
    IConsentKeyFiles const& files,
    core::IRandom& random,
    std::filesystem::path const& directory,
    std::filesystem::path const& socket,
    uint64_t daemonInstance);

/// Reads the key the run @p daemonInstance of the daemon at @p socket keeps in @p directory.
/// @return The key, or why it cannot be read or is refused.
[[nodiscard]] std::expected<ConsentKey, std::string> readDaemonConsentKey(
    IConsentKeyFiles const& files,
    std::filesystem::path const& directory,
    std::filesystem::path const& socket,
    uint64_t daemonInstance);

/// @return HMAC-SHA256 of @p key over a fixed label, @p context's run and socket, and @p nonce.
[[nodiscard]] ConsentProofMac consentProofOf(ConsentKey const& key,
                                             ConsentContext const& context,
                                             ConsentNonce const& nonce);

/// Checks a window's proof, in constant time.
/// @return Proven when @p proof is consentProofOf(@p key, @p context, @p nonce).
[[nodiscard]] ConsentProofCheck checkConsentProof(ConsentKey const& key,
                                                  ConsentContext const& context,
                                                  ConsentNonce const& nonce,
                                                  ConsentProofMac const& proof);

} // namespace vthost
