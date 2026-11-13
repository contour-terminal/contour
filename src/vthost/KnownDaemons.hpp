// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The daemons this user has trusted on first use: one line per `HOST:PORT`, holding the fingerprint of the
/// certificate the daemon presented then (`known_daemons`, as ssh's `known_hosts` is for hosts). A TLS
/// connection to a daemon that has no CA to verify it against is trusted when what it presents is what was
/// pinned, and refused when it is not.

#include <core/Random.hpp>
#include <core/async/Task.hpp>
#include <core/platform/FileSystem.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vthost
{

/// @param configDirectory The user's Contour configuration directory, which is theirs alone. Never the state
///        directory: that one falls back to a shared temporary directory, where another local user could have
///        planted a pin.
/// @return `<configDirectory>/known_daemons`, where a client keeps the daemons it pinned, as ssh keeps
///         known_hosts.
[[nodiscard]] inline std::filesystem::path knownDaemonsPath(std::filesystem::path const& configDirectory)
{
    return configDirectory / "known_daemons";
}

/// @param host A daemon's host: a name, an IPv4 address or a bare IPv6 address.
/// @param port Its port.
/// @return The key a daemon is pinned under: `HOST:PORT`, an IPv6 address bracketed (`[::1]:7297`), the
///         spelling `parseHostPort()` reads back as @p host and @p port.
[[nodiscard]] std::string pinKey(std::string_view host, std::uint16_t port);

/// @param spec A daemon as the user spells it, `HOST:PORT` or `[v6]:PORT`.
/// @return The key it is pinned under (pinKey()), so that every spelling of one daemon names one pin;
///         nullopt for a @p spec that does not parse.
[[nodiscard]] std::optional<std::string> pinKeyOf(std::string_view spec);

/// The user's answer to "trust this daemon from now on?".
enum class PinDecision : std::uint8_t
{
    Decline = 0, ///< Do not connect.
    Trust,       ///< Pin the fingerprint and go on.
};

/// Asks the user whether to pin a daemon met for the first time. Without one, a first contact can only
/// succeed when the fingerprint was given beforehand (`--daemon-fingerprint`).
class IPinPrompt
{
  public:
    virtual ~IPinPrompt() = default;

    /// @param hostPort The daemon, as `HOST:PORT`.
    /// @param fingerprint What it presents, as `SHA256:<base64>`.
    /// @return What the user decided.
    [[nodiscard]] virtual core::async::Task<PinDecision> confirm(std::string hostPort,
                                                                 std::string fingerprint) = 0;
};

/// One pinned daemon.
struct DaemonPin
{
    std::string hostPort;    ///< `HOST:PORT`.
    std::string fingerprint; ///< `SHA256:<base64>`.
};

/// The pinned daemons, kept in a file. Every call reads the file afresh and a write replaces it whole, so
/// that two processes pinning at the same time lose at most the race itself, never the file.
class KnownDaemons
{
  public:
    /// @param fileSystem What the file is read and written through; must outlive this object.
    /// @param file Where the pins live; a missing file is no pins.
    /// @param random Names each write's temporary file, so that two writers never share one; must outlive
    ///        this object.
    KnownDaemons(core::platform::FileSystem const& fileSystem,
                 std::filesystem::path file,
                 core::IRandom& random);

    /// @param hostPort A daemon, as pinKey() spells it.
    /// @return The fingerprint pinned for it, nullopt when none is (a missing file pins nothing), or why the
    ///         file cannot be read. An unreadable file is an error, never "no pin": taken for none, every
    ///         daemon -- a changed one too -- would be a first contact the user is asked about. A line that
    ///         does not parse is skipped, never fatal: a damaged line must not lock the user out of every
    ///         daemon.
    [[nodiscard]] std::expected<std::optional<std::string>, std::string> pinOf(
        std::string_view hostPort) const;

    /// @return The file the pins are kept in.
    [[nodiscard]] std::filesystem::path const& file() const noexcept { return _file; }

    /// Pins @p fingerprint for @p hostPort, replacing any earlier pin of that daemon.
    /// @param hostPort A daemon, as `HOST:PORT`.
    /// @param fingerprint What it presents, as `SHA256:<base64>`.
    /// @return Nothing, or why the file could not be written.
    [[nodiscard]] std::expected<void, std::string> pin(std::string_view hostPort,
                                                       std::string_view fingerprint);

    /// Removes the pin of @p hostPort.
    /// @param hostPort A daemon, as `HOST:PORT`.
    /// @return Whether there was a pin to remove, or why the file could not be written.
    [[nodiscard]] std::expected<bool, std::string> forget(std::string_view hostPort);

  private:
    /// @return Every well-formed line; none for a missing file; an error for a file that cannot be read.
    [[nodiscard]] std::expected<std::vector<DaemonPin>, std::string> load() const;

    /// Replaces the file with @p entries, atomically: written beside it, synced and renamed over it.
    [[nodiscard]] std::expected<void, std::string> store(std::vector<DaemonPin> const& entries) const;

    core::platform::FileSystem const& _fileSystem;
    std::filesystem::path _file;
    core::IRandom& _random;
};

} // namespace vthost
