// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/cli/CLI.hpp>
#include <core/codec/Fields.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <vthost/DaemonLifecycle.hpp>

namespace contour::cli
{

/// Whether a `contour daemon` switch was given on the command line.
///
/// A named enumeration where a `bool` would do, as the guidelines ask: the CLI parser delivers a switch as a
/// `bool`, and this is where it becomes a value that says what it means.
enum class OptionFlag : uint8_t
{
    Absent = 0, ///< The switch was not given.
    Given,      ///< The switch was given.
};

/// Every option `contour daemon` reads, in one record, as the command line gave them.
///
/// The record is the daemon verb's whole input: ContourApp reads it first and uses nothing else. It also
/// crosses to the PTY keeper as an opaque byte record (@see encodeDaemonLaunch), so that the keeper can
/// start the daemon again with the options it started with.
///
/// The values are what the user wrote, not what they resolve to: an empty string is an option not given, and
/// a value the daemon rejects (an unknown `--lifecycle`) is rejected by the daemon, as it was before the
/// record existed. Read once and never changed: what is derived from it is DaemonLaunch's.
struct DaemonVerbOptions
{
    std::string socket;                         ///< `--socket`: the control socket file.
    std::string label;                          ///< `--label`: the socket label.
    std::string config;                         ///< `--config`: the configuration file.
    std::string profile;                        ///< `--profile`: the profile the hosted sessions run.
    OptionFlag background = OptionFlag::Absent; ///< `--background`: relaunch detached.
    OptionFlag stdio = OptionFlag::Absent;      ///< `--stdio`: relay to a daemon instead of becoming one.
    OptionFlag inPlace = OptionFlag::Absent;    ///< `--in-place`: run from the installed binary.
    std::string handover; ///< `--handover`: what a live upgrade started this daemon with.
    std::string lifecycle { vthost::nameOf(vthost::DaemonLifecycle::Persistent) }; ///< `--lifecycle`.
    OptionFlag exitWithLastSession = OptionFlag::Absent; ///< `--exit-with-last-session`.
    std::string sizePolicy;                              ///< `--size-policy`.
    std::string tmuxCompatSocket; ///< `--tmux-compat-socket`: the label of tmux's discovery path.
    std::string listenTcp;        ///< `--listen-tcp`: HOST:PORT of the TCP listener.
    std::string token;            ///< `--token`: the preshared TCP token.
    std::string tokenFile;        ///< `--token-file`: the file the TCP token is read from.
    std::string accessFile;       ///< `--access-file`: the narrower TCP tokens.
    std::string tlsCert;          ///< `--tls-cert`: the TCP listener's certificate.
    std::string tlsKey;           ///< `--tls-key`: the TCP listener's private key.
    std::string log;              ///< `--log`: the log tag filter.
    std::string logFile;          ///< `--log-file`: where the log goes; `-` for standard error.
    std::string authorizedKeys;   ///< `--authorized-keys`: the keys that may log in over TCP by signature.
    std::string auth;             ///< `--auth`: which TCP credentials the listener accepts.

    /// The wire ids of the fields: stable, never reused or renumbered. A field is added at the end, with the
    /// next id after the last one DaemonLaunch uses; a record an older release wrote lacks it, and decodes
    /// with its default.
    /// Every member of the struct has exactly one row, and @ref DaemonOptionFieldCount says how many.
    static constexpr auto Fields = std::tuple {
        std::pair { 1U, &DaemonVerbOptions::socket },
        std::pair { 2U, &DaemonVerbOptions::label },
        std::pair { 3U, &DaemonVerbOptions::config },
        std::pair { 4U, &DaemonVerbOptions::profile },
        std::pair { 5U, &DaemonVerbOptions::background },
        std::pair { 6U, &DaemonVerbOptions::stdio },
        std::pair { 7U, &DaemonVerbOptions::inPlace },
        std::pair { 8U, &DaemonVerbOptions::handover },
        std::pair { 9U, &DaemonVerbOptions::lifecycle },
        std::pair { 10U, &DaemonVerbOptions::exitWithLastSession },
        std::pair { 11U, &DaemonVerbOptions::sizePolicy },
        std::pair { 12U, &DaemonVerbOptions::tmuxCompatSocket },
        std::pair { 13U, &DaemonVerbOptions::listenTcp },
        std::pair { 14U, &DaemonVerbOptions::token },
        std::pair { 15U, &DaemonVerbOptions::tokenFile },
        std::pair { 16U, &DaemonVerbOptions::accessFile },
        std::pair { 17U, &DaemonVerbOptions::tlsCert },
        std::pair { 18U, &DaemonVerbOptions::tlsKey },
        std::pair { 19U, &DaemonVerbOptions::log },
        std::pair { 20U, &DaemonVerbOptions::logFile },
        std::pair { 21U, &DaemonVerbOptions::authorizedKeys },
        std::pair { 22U, &DaemonVerbOptions::auth },
    };

    /// Lists the fields for the codec.
    void visitFields(this auto& self, auto& visitor)
    {
        std::apply([&](auto const&... row) { (visitor(row.first, self.*(row.second)), ...); }, Fields);
    }

    [[nodiscard]] bool operator==(DaemonVerbOptions const&) const = default;
};

/// How many members DaemonVerbOptions has, and so how many rows its codec lists.
inline constexpr std::size_t DaemonOptionFieldCount = 22;

static_assert(std::tuple_size_v<decltype(DaemonVerbOptions::Fields)> == DaemonOptionFieldCount,
              "a member of DaemonVerbOptions was added or removed without its row in Fields");

/// Compile-time guard, never called: a structured binding names every member, so it stops compiling the
/// moment a member is added to DaemonVerbOptions. The fix is the same as for @ref DaemonVerbOptions::Fields:
/// give the member its row, bump @ref DaemonOptionFieldCount, extend this binding, and extend the round-trip
/// test.
inline void requireEveryDaemonOptionListed(DaemonVerbOptions const& options)
{
    [[maybe_unused]] auto const& [socket,
                                  label,
                                  config,
                                  profile,
                                  background,
                                  stdio,
                                  inPlace,
                                  handover,
                                  lifecycle,
                                  exitWithLastSession,
                                  sizePolicy,
                                  tmuxCompatSocket,
                                  listenTcp,
                                  token,
                                  tokenFile,
                                  accessFile,
                                  tlsCert,
                                  tlsKey,
                                  log,
                                  logFile,
                                  authorizedKeys,
                                  auth] = options;
}

/// What `contour daemon` launches a daemon with: the options as given, and what they resolve to.
///
/// Made in one step (resolve()), so that none exists half-resolved; it is what the options record encodes
/// (@see encodeDaemonLaunch) and what the PTY keeper hands every daemon it starts.
class DaemonLaunch
{
  public:
    /// Resolves @p given against the configuration.
    /// @param given The options as the command line gave them.
    /// @param configuredSocket The configuration's `daemon.socket`; empty for none.
    /// @return The launch: the control socket is `--socket`, else the configuration's `daemon.socket`, else
    ///         the one the label (`--label`, `$CONTOUR_MUX`, or the default) names.
    [[nodiscard]] static DaemonLaunch resolve(DaemonVerbOptions given, std::string const& configuredSocket);

    /// Restores a launch exactly as it was resolved; what decodeDaemonLaunch() makes of a record.
    /// @param given The options as given.
    /// @param socketPath The control socket they resolved to.
    /// @return The launch.
    [[nodiscard]] static DaemonLaunch restore(DaemonVerbOptions given, std::filesystem::path socketPath);

    /// @return The options as the command line gave them.
    [[nodiscard]] DaemonVerbOptions const& given() const noexcept { return _given; }

    /// @return The control socket the daemon binds.
    [[nodiscard]] std::filesystem::path const& socketPath() const noexcept { return _socketPath; }

    [[nodiscard]] bool operator==(DaemonLaunch const&) const = default;

  private:
    DaemonLaunch(DaemonVerbOptions given, std::filesystem::path socketPath):
        _given { std::move(given) }, _socketPath { std::move(socketPath) }
    {
    }

    DaemonVerbOptions _given;
    std::filesystem::path _socketPath;
};

/// The wire id of DaemonLaunch::socketPath in the options record: after every DaemonVerbOptions field.
inline constexpr uint32_t DaemonLaunchSocketPathField = 23;

/// The version of the options record this release writes. A reader refuses a record of a newer version; it
/// reads every older one.
inline constexpr uint32_t DaemonOptionsVersion = 1;

/// Why an options record could not be decoded.
enum class DaemonOptionsError : uint8_t
{
    Truncated = 0, ///< The record ends inside a value or a frame.
    NewerVersion,  ///< The record was written by a newer release than this reader.
    Malformed,     ///< The record is not well-formed, or holds a value out of range.
};

/// Reads the `contour daemon` options from a parsed command line.
/// @param parameters The parsed command line, holding every option of `contour daemon`.
/// @return The options as given.
[[nodiscard]] DaemonVerbOptions readDaemonVerbOptions(core::cli::FlagStore const& parameters);

/// Writes @p launch as an opaque, versioned byte record (the options record): a header (the version), then
/// one frame of fields, the options' and the resolved socket's.
///
/// The record holds `--token`: it is never logged and never written to a file; it lives in the keeper's
/// memory and crosses the keeper's private channel only.
/// @param launch The launch.
/// @return The record.
[[nodiscard]] std::vector<uint8_t> encodeDaemonLaunch(DaemonLaunch const& launch);

/// Reads a record encodeDaemonLaunch() wrote, of any version up to #DaemonOptionsVersion.
/// @param record The record.
/// @return The launch, or why @p record is no record this release can read.
[[nodiscard]] std::expected<DaemonLaunch, DaemonOptionsError> decodeDaemonLaunch(
    std::span<uint8_t const> record);

} // namespace contour::cli

namespace core::codec
{

/// The wire table of OptionFlag: appended to, never reordered.
template <>
struct EnumWire<contour::cli::OptionFlag>
{
    static constexpr auto Values = std::array {
        contour::cli::OptionFlag::Absent,
        contour::cli::OptionFlag::Given,
    };
};

} // namespace core::codec
