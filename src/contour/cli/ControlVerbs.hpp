// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The control CLI: `contour ls | new | split | send | pane capture | close | label | api ...`. Every verb is
/// one row of `controlVerbs()`; the command list, the dispatch, the help and the agent skill all read that
/// table, so a verb is a row, not an edit in four places. A verb turns its flags into the JSON of one
/// catalog method, calls it through a `ControlChannel`, and prints the reply for people or, with `--json`,
/// as received.

#include <contour/cli/ExitStatus.hpp>
#include <contour/config/HostUrl.hpp>

#include <core/Environment.hpp>
#include <core/async/StopToken.hpp>
#include <core/cli/CLI.hpp>
#include <core/platform/Clock.hpp>
#include <core/platform/FileSystem.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <iosfwd>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <vtcontrol/Api.hpp>
#include <vtcontrol/ControlError.hpp>
#include <vthost/client/ControlClient.hpp>

namespace vthost
{
class IStdioSpawner;
}

namespace contour::cli
{

/// Whether the verb's standard output is a person's terminal.
enum class OutputTarget : uint8_t
{
    Pipe = 0, ///< A pipe or a file: output is meant for programs.
    Terminal, ///< A terminal: output is meant for a person.
};

/// How the last argument of a verb that takes free-form input is read.
enum class InputExpansion : uint8_t
{
    None = 0,    ///< As typed.
    Stdin,       ///< `-` is replaced by standard input.
    StdinOrFile, ///< `-` is replaced by standard input, and `@FILE` by the file's contents.
};

/// Whether a verb makes one call or follows a stream of events.
enum class VerbMode : uint8_t
{
    Call = 0, ///< One request, one reply.
    Stream,   ///< A subscription: printHuman is called with each event until the stream ends.
};

/// Whether a verb tells the daemon which pane it was typed in.
enum class ContextPolicy : uint8_t
{
    Send = 0, ///< `$CONTOUR_PANE` goes along as the call's context.
    Omit,     ///< The call carries no context.
};

/// How a verb's reply reads for a person.
enum class HumanForm : uint8_t
{
    Text = 0, ///< Written by the verb's printHuman.
    Json,     ///< The reply's JSON itself: indented on a terminal, one line otherwise.
};

/// Where a verb's method comes from.
enum class MethodSource : uint8_t
{
    Fixed = 0, ///< The row's `method` (empty for a local verb).
    FirstWord, ///< The first trailing word, as typed (`api call NAME`).
};

/// One row per control verb.
struct ControlVerb
{
    std::string_view path;    ///< The link suffix: "ls", "block.list", "api.call".
    std::string_view method;  ///< The catalog method; empty for a local verb, which never reaches a daemon.
    std::string_view usage;   ///< The usage line, without `contour `: "split [--pane P] [--right | --below]".
    std::string_view summary; ///< A one-line summary.
    core::cli::OptionList (*options)();          ///< The verb's options, the connection options included.
    std::optional<core::cli::Verbatim> verbatim; ///< The trailing words the verb takes, if any.
    /// @param flags The parsed command line.
    /// @return The request JSON, or why the command line is malformed (reported as bad usage).
    std::expected<std::string, std::string> (*buildParams)(core::cli::FlagStore const& flags);
    /// Prints the reply for people. A local verb is called with an empty reply.
    void (*printHuman)(std::string_view replyJson, std::ostream& out);

    MethodSource methodSource = MethodSource::Fixed; ///< Where the method is taken from.
    std::string_view alternateMethod = {}; ///< The method used instead when usesAlternateMethod says so.
    /// @return Whether @p flags select the alternate method (`close --tab`); null for never.
    bool (*usesAlternateMethod)(core::cli::FlagStore const& flags) = nullptr;
    ContextPolicy context = ContextPolicy::Send; ///< Whether the caller's pane is sent along.
    HumanForm human = HumanForm::Text;           ///< How the reply reads for a person.
    InputExpansion input = InputExpansion::None; ///< How the last word is read.
    std::string_view
        briefFlag = {}; ///< A boolean flag that selects printBrief over printHuman; empty for none.
    void (*printBrief)(std::string_view replyJson, std::ostream& out) = nullptr; ///< The terse reply.
    /// @param replyJson The reply.
    /// @param err Where a note about the status goes.
    /// @return The process exit status the reply stands for (`send --wait` exits with its command's); null
    ///         for Ok.
    int (*replyExitStatus)(std::string_view replyJson, std::ostream& err) = nullptr;
    std::string_view timeoutFlag = {}; ///< A seconds-valued flag that bounds the call; empty for none.
    VerbMode mode = VerbMode::Call;    ///< Whether the verb follows a stream.
    std::string_view
        followFlag = {}; ///< For a stream: the boolean flag that keeps it going; empty to follow.
};

/// Everything a control verb reads from and writes to besides its flags.
struct ControlIo
{
    core::Environment const& env;             ///< Where `$CONTOUR_PANE` and `$CONTOUR_DAEMON` are read.
    core::platform::FileSystem& fs;           ///< Where `--token-file`, `--tls-ca` and `@FILE` are read.
    std::istream& in;                         ///< Standard input, for `-`.
    std::ostream& out;                        ///< Standard output.
    std::ostream& err;                        ///< Standard error.
    core::platform::IWallClock const& clock;  ///< Stamps the lines of an event stream.
    OutputTarget target = OutputTarget::Pipe; ///< What @ref out is.
    core::async::StopToken stop {};           ///< Requested on Ctrl+C: ends a stream.
    /// Resolves `--daemon NAME` and `--daemon ssh://...` (@ref makeSavedHostResolver); empty for no saved
    /// hosts, every such spec then being a socket path.
    vthost::client::SavedHostResolver resolveSavedHost {};
};

/// The daemon's end of a control verb: where its calls go. The real one dials a transport; tests answer.
class ControlChannel
{
  public:
    virtual ~ControlChannel() = default;

    /// Calls one method.
    /// @param method The catalog method.
    /// @param params The request JSON.
    /// @param context The caller's pane id, or empty.
    /// @param timeout How long the call may take; none for as long as the daemon takes.
    /// @return The reply JSON, or why there is none.
    [[nodiscard]] virtual std::expected<std::string, vthost::client::ControlClientError> call(
        std::string_view method,
        std::string_view params,
        std::string_view context,
        std::optional<std::chrono::milliseconds> timeout) = 0;

    /// Subscribes and hands every event to @p onEvent until it says Stop, @p stop is requested, or the
    /// connection ends.
    /// @param params The `events.subscribe` request JSON.
    /// @param context The caller's pane id, or empty.
    /// @param onEvent Called with each event's JSON.
    /// @param stop Ends the stream when requested.
    /// @return Nothing when the stream ended by Stop or @p stop, or why it ended otherwise.
    [[nodiscard]] virtual std::expected<void, vthost::client::ControlClientError> stream(
        std::string_view params,
        std::string_view context,
        std::function<vthost::client::StreamStep(std::string_view eventJson)> onEvent,
        core::async::StopToken const& stop) = 0;
};

/// @param transport How to reach the daemon.
/// @return A channel that connects, handshakes and calls through @p transport, once per call.
[[nodiscard]] std::unique_ptr<ControlChannel> makeTransportChannel(
    std::shared_ptr<vthost::ITransportFactory> transport);

/// Finds the host a `--daemon` spec names: a saved one of `hosts:`, or an ad-hoc URL. `config::resolveHost`
/// in the CLI; a table in a test.
using HostLookup = std::function<std::expected<config::HostEntry, std::string>(std::string_view nameOrUrl)>;

/// How `--daemon NAME` and `--daemon ssh://...` reach a remote host, as `contour client --host` does: the
/// host is looked up, then mapped onto its transport by `makeHostTransport`.
/// @param lookup Finds the host.
/// @param fileSystem Where a host's token_file and tls_ca are read. Must outlive the resolver.
/// @param spawner Starts ssh, for an ssh host.
/// @return The resolver, whose refusal names the host and why it cannot be reached as configured.
[[nodiscard]] vthost::client::SavedHostResolver makeSavedHostResolver(
    HostLookup lookup,
    core::platform::FileSystem const& fileSystem,
    std::shared_ptr<vthost::IStdioSpawner> spawner);

/// @param code What the daemon answered.
/// @return BadRequest is BadUsage; NotFound and Ambiguous are TargetNotFound; everything else
///         (Forbidden and LimitReached included) is Failed.
[[nodiscard]] ExitStatus exitStatusFor(vtcontrol::ControlErrorCode code) noexcept;

/// @param failure Why a call did not get its reply.
/// @return Unreachable and Disconnected are Unreachable; Misconfigured (a --tls-ca that does not parse) is
///         BadUsage; the rest are Failed. A Remote failure is judged by its code instead: see the overload
///         taking the error.
[[nodiscard]] ExitStatus exitStatusFor(vthost::client::ControlFailure failure) noexcept;

/// @param error A failed call.
/// @return The daemon's code judged by exitStatusFor(ControlErrorCode) for a Remote failure, the failure
///         judged by exitStatusFor(ControlFailure) otherwise.
[[nodiscard]] ExitStatus exitStatusFor(vthost::client::ControlClientError const& error) noexcept;

/// @param block A command block.
/// @return Its exit code clamped to 0..255; 1 where it has none, is negative, or was superseded.
[[nodiscard]] int exitStatusForBlock(vtcontrol::BlockInfo const& block) noexcept;

/// @param block A command block.
/// @return Why exitStatusForBlock could not use the block's exit code, or nothing when it could.
[[nodiscard]] std::optional<std::string> exitStatusNote(vtcontrol::BlockInfo const& block);

/// @return Every control verb, in help order.
[[nodiscard]] std::span<ControlVerb const> controlVerbs();

/// The groups the dotted paths of controlVerbs() hang under (`block` for `block.list`).
struct ControlVerbGroup
{
    std::string_view name;     ///< The group's CLI name.
    std::string_view helpText; ///< Its help text.
};

/// @return The groups, for the command list.
[[nodiscard]] std::span<ControlVerbGroup const> controlVerbGroups();

/// @param verb A control verb.
/// @return The method it calls given @p flags, which is empty for a local verb.
[[nodiscard]] std::string_view methodFor(ControlVerb const& verb, core::cli::FlagStore const& flags);

/// Runs a verb against the daemon its flags and the environment name.
///
/// A local verb (empty method) never resolves an endpoint: its flags are validated, and it prints.
/// `--json` is ignored for it.
/// @param verb The verb.
/// @param flags The parsed command line.
/// @param io Environment, files and streams.
/// @return The process exit status.
[[nodiscard]] int runControlVerb(ControlVerb const& verb,
                                 core::cli::FlagStore const& flags,
                                 ControlIo const& io);

/// Runs a verb over @p channel instead of resolving a daemon.
/// @param verb The verb.
/// @param flags The parsed command line.
/// @param channel Where the verb's call goes.
/// @param io Environment, files and streams.
/// @return The process exit status.
[[nodiscard]] int runControlVerb(ControlVerb const& verb,
                                 core::cli::FlagStore const& flags,
                                 ControlChannel& channel,
                                 ControlIo const& io);

} // namespace contour::cli
