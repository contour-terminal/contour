// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// When Contour offers to install shell integration, what the offer says, and the line it would add to
/// the shell's startup file. Pure: the session feeds the probe, the manager asks offerFor(), the card
/// shows CardStageRows, and the tests read the same rows.

#include <contour/config/ExecutionPolicy.hpp>
#include <contour/config/ShellKind.hpp>
#include <contour/config/WindowControlStyle.hpp>
#include <contour/window/DroppedText.hpp> // detail::PowerShellQuotes

#include <vtbackend/core/Primitives.hpp>

#include <core/Environment.hpp>
#include <core/Utf8Path.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <ranges>
#include <string>
#include <string_view>

namespace contour::window
{

/// How many commands entered on the primary screen, with no OSC 133 mark seen, make the offer.
inline constexpr int ShellIntegrationProbeCommands = 3;

/// Whether the terminal has seen any OSC 133 prompt mark.
enum class PromptMarks : uint8_t
{
    Absent = 0,
    Seen,
};

/// What the probe concludes after a command.
enum class ProbeVerdict : uint8_t
{
    Waiting = 0, ///< Not enough evidence yet.
    Offer,       ///< The threshold was just crossed with no prompt marks: offer once.
    Integrated,  ///< The shell marks its prompts: nothing to offer, for the session's life.
};

/// Counts the commands a session has been given and decides when the lack of prompt marks is worth
/// an offer. One per session.
class ShellIntegrationProbe
{
  public:
    /// Notes one command, a Return the session wrote to the PTY.
    /// @param screen The screen it was entered on; the alternate screen (vim, less) does not count.
    /// @param marks Whether the terminal holds any prompt mark by now.
    /// @return Integrated for every call once @p marks was Seen; Offer exactly once, on the
    ///         ShellIntegrationProbeCommands-th primary-screen call without marks; Waiting otherwise.
    [[nodiscard]] ProbeVerdict noteCommandEntered(vtbackend::ScreenType screen, PromptMarks marks) noexcept
    {
        if (_integrated || marks == PromptMarks::Seen)
        {
            _integrated = true;
            return ProbeVerdict::Integrated;
        }
        if (screen != vtbackend::ScreenType::Primary)
            return ProbeVerdict::Waiting;
        ++_commands;
        return _commands == ShellIntegrationProbeCommands ? ProbeVerdict::Offer : ProbeVerdict::Waiting;
    }

  private:
    int _commands = 0;
    bool _integrated = false;
};

/// Whether the shell's startup file is read on this machine.
enum class RcLocality : uint8_t
{
    Remote = 0, ///< The window is attached to another host: its shells read that host's files.
    ThisMachine,
};

/// Whether the user said "Not Now" for this shell.
enum class OfferDismissal : uint8_t
{
    Open = 0,
    Dismissed,
};

/// Whether this run of the app has shown the card already.
enum class CardSlot : uint8_t
{
    Free = 0,
    Taken,
};

/// What to show the user.
enum class ShellIntegrationOffer : uint8_t
{
    None = 0,
    Card,
};

/// Whether the path the running binary can be named by is still there for a shell started later, on this
/// machine's own terms. An rc line outlives this process, so a path that dies with it is no use.
enum class BinaryReach : uint8_t
{
    Unreachable = 0, ///< Only this process can see it (a Flatpak sandbox path): no line can name it.
    Reachable,       ///< Every later shell finds it there.
};

/// The environment variables that say where the running binary really lives.
struct BinaryEnvironment
{
    std::string
        appImage {}; ///< `APPIMAGE`: the AppImage file, which outlives the mount the binary runs from.
    std::string flatpakId {}; ///< `FLATPAK_ID`: set inside a Flatpak sandbox, whose paths the host lacks.
};

/// The running binary as an rc line must name it.
struct LocatedBinary
{
    std::filesystem::path path {}; ///< The path the line runs.
    BinaryReach reach = BinaryReach::Unreachable;
};

/// Reads the variables locateBinary() decides on.
/// @param env The environment.
/// @return Their values; empty for an unset one.
[[nodiscard]] inline BinaryEnvironment binaryEnvironmentOf(core::Environment const& env)
{
    return { .appImage = env.get("APPIMAGE").value_or(""), .flatpakId = env.get("FLATPAK_ID").value_or("") };
}

/// Decides which path an rc line names for the running binary.
///
/// A Flatpak is not offered a line at all, rather than a `flatpak run` one: the line is a quoted path
/// that the guard and `generate integration` command are built around, and a command with arguments
/// would not keep that quoting proof.
/// @param env Where the binary lives.
/// @param applicationFile What the process was started as (`QCoreApplication::applicationFilePath()`).
/// @return Inside a Flatpak, @p applicationFile and Unreachable; under an AppImage, the AppImage file
///         (the mount @p applicationFile is in vanishes on exit) and Reachable; else @p applicationFile
///         and Reachable.
[[nodiscard]] inline LocatedBinary locateBinary(BinaryEnvironment const& env,
                                                std::filesystem::path const& applicationFile)
{
    if (!env.flatpakId.empty())
        return { .path = applicationFile, .reach = BinaryReach::Unreachable };
    if (!env.appImage.empty())
        return { .path = std::filesystem::path { env.appImage }, .reach = BinaryReach::Reachable };
    return { .path = applicationFile, .reach = BinaryReach::Reachable };
}

/// What stops Install from being offered for a shell, whichever surface asks.
enum class InstallBlock : uint8_t
{
    None = 0,          ///< Nothing stops it.
    NotOnWindows,      ///< A POSIX shell's startup files are not read on a Windows host.
    WindowsOnly,       ///< A Windows shell has no startup file off Windows.
    BinaryUnreachable, ///< The line could not name a binary a later shell finds.
    GroupPolicy,       ///< The organisation's PowerShell policy forbids profile scripts.
    SignedScriptsOnly, ///< PowerShell runs only signed scripts here, and the profile is not.
};

/// The words of one block, shown by the Settings page in place of the status of a file it cannot install to.
struct InstallBlockRow
{
    InstallBlock block = InstallBlock::None;
    std::string_view text {};
};

/// Every block, in enumerator order.
inline constexpr auto InstallBlockRows = std::array {
    InstallBlockRow { .block = InstallBlock::None, .text = "" },
    InstallBlockRow { .block = InstallBlock::NotOnWindows, .text = "Not available on Windows" },
    InstallBlockRow { .block = InstallBlock::WindowsOnly, .text = "Only available on Windows" },
    InstallBlockRow { .block = InstallBlock::BinaryUnreachable,
                      .text = "Not available here: shells outside this sandbox cannot run this Contour" },
    InstallBlockRow { .block = InstallBlock::GroupPolicy,
                      .text = "Blocked by your organisation's PowerShell policy" },
    InstallBlockRow { .block = InstallBlock::SignedScriptsOnly,
                      .text = "PowerShell requires signed scripts here" },
};

/// The one decision both the card and the Settings page ask.
/// @param shell The shell.
/// @param host The host.
/// @param binary Whether the running binary can be named by an rc line.
/// @param scripts Whether the shell's profile runs under its execution policy (Run for a shell without one).
/// @return What blocks Install, host first, then binary, then policy; None when nothing does.
[[nodiscard]] constexpr InstallBlock installBlockOf(config::ShellRow const& shell,
                                                    config::HostPlatform host,
                                                    BinaryReach binary,
                                                    config::ProfileScripts scripts) noexcept
{
    if (!config::isInstallableOn(shell, host))
        return host == config::HostPlatform::Windows ? InstallBlock::NotOnWindows : InstallBlock::WindowsOnly;
    if (binary == BinaryReach::Unreachable)
        return InstallBlock::BinaryUnreachable;
    switch (scripts)
    {
        case config::ProfileScripts::BlockedByGroupPolicy: return InstallBlock::GroupPolicy;
        case config::ProfileScripts::SignedOnly: return InstallBlock::SignedScriptsOnly;
        case config::ProfileScripts::Run:
        case config::ProfileScripts::NeedUserPolicy: break;
    }
    return InstallBlock::None;
}

/// Everything offerFor() decides on. The defaults are the "offer nothing" answers.
struct OfferInputs
{
    config::ShellKind shell = config::ShellKind::Unknown;
    RcLocality locality = RcLocality::Remote;
    config::HostPlatform host = config::HostPlatform::Other;
    BinaryReach binary = BinaryReach::Unreachable;
    OfferDismissal dismissal = OfferDismissal::Open;
    config::RcState rc = config::RcState::Unknown;
    CardSlot slot = CardSlot::Free;
    config::ProfileScripts scripts = config::ProfileScripts::Run; ///< Whether the profile runs (PowerShell).
};

/// Decides whether the card shows.
/// @param in The facts.
/// @return Card iff the shell is known, its startup file is on this machine, installBlockOf() finds
///         nothing in the way (the shell fits the host, a binary a later shell can run, a policy that lets
///         the profile run or be made to), the shell was not dismissed, its file does not load Contour's
///         script, and no card showed yet.
[[nodiscard]] constexpr ShellIntegrationOffer offerFor(OfferInputs const& in) noexcept
{
    auto const* const row = config::shellRow(in.shell);
    auto const offered = row != nullptr && in.locality == RcLocality::ThisMachine
                         && installBlockOf(*row, in.host, in.binary, in.scripts) == InstallBlock::None
                         && in.dismissal == OfferDismissal::Open && in.rc == config::RcState::Absent
                         && in.slot == CardSlot::Free;
    return offered ? ShellIntegrationOffer::Card : ShellIntegrationOffer::None;
}

/// The stages of the card.
enum class CardStage : uint8_t
{
    Hidden = 0,
    Offer,   ///< "Turn on command blocks".
    Confirm, ///< The exact line, before anything is written.
};

/// The words of one stage. `{shell}`, `{rc}` and `{line}` are replaced by cardText().
struct CardStageRow
{
    CardStage stage = CardStage::Hidden;
    std::string_view title {};
    std::string_view body {};
    std::string_view primary {};
    std::string_view secondary {};
};

/// Every stage, in enumerator order.
inline constexpr auto CardStageRows = std::array {
    CardStageRow { .stage = CardStage::Hidden, .title = "", .body = "", .primary = "", .secondary = "" },
    CardStageRow { .stage = CardStage::Offer,
                   .title = "Turn on command blocks",
                   .body = "Install shell integration for {shell} to fold output, see exit codes and jump "
                           "between commands.",
                   .primary = "Install",
                   .secondary = "Not Now" },
    CardStageRow { .stage = CardStage::Confirm,
                   .title = "Add this line to {rc}?",
                   .body = "{line}",
                   .primary = "Add Line",
                   .secondary = "Cancel" },
};

/// The confirm step's extra sentence when Install also fixes the execution policy. `{shell}` is replaced.
inline constexpr std::string_view PolicyFixNote =
    "This also lets your own scripts run in {shell}, for your user only (Set-ExecutionPolicy -Scope "
    "CurrentUser RemoteSigned).";

/// The confirm step's primary button when Install also fixes the execution policy.
inline constexpr std::string_view PolicyFixPrimary = "Allow Scripts and Add Line";

/// @param scripts Whether the profile runs under the shell's execution policy.
/// @return PolicyFixNote when the policy must be changed first, else empty.
[[nodiscard]] constexpr std::string_view policyNoteFor(config::ProfileScripts scripts) noexcept
{
    return scripts == config::ProfileScripts::NeedUserPolicy ? PolicyFixNote : std::string_view {};
}

/// @param scripts Whether the profile runs under the shell's execution policy.
/// @return PolicyFixPrimary when the policy must be changed first, else the Confirm stage's primary.
[[nodiscard]] constexpr std::string_view confirmPrimaryFor(config::ProfileScripts scripts) noexcept
{
    return scripts == config::ProfileScripts::NeedUserPolicy
               ? PolicyFixPrimary
               : CardStageRows[std::to_underlying(CardStage::Confirm)].primary;
}

/// Fills a card pattern in.
/// @param pattern Text holding `{shell}`, `{rc}` and `{line}` placeholders. Substituted text is never
///        scanned again, so a line that contains `{rc}` stays as it is.
/// @param shell The shell's name.
/// @param rc The startup file as the user would say it.
/// @param line The line that would be added.
/// @return The text.
[[nodiscard]] inline std::string cardText(std::string_view pattern,
                                          std::string_view shell,
                                          std::string_view rc,
                                          std::string_view line)
{
    struct Placeholder
    {
        std::string_view token;
        std::string_view value;
    };
    auto const placeholders = std::array { Placeholder { "{shell}", shell },
                                           Placeholder { "{rc}", rc },
                                           Placeholder { "{line}", line } };

    auto result = std::string {};
    while (!pattern.empty())
    {
        auto const match = std::ranges::find_if(
            placeholders, [&](Placeholder const& p) { return pattern.starts_with(p.token); });
        if (match != placeholders.end())
        {
            result += match->value;
            pattern.remove_prefix(match->token.size());
        }
        else
        {
            result += pattern.front();
            pattern.remove_prefix(1);
        }
    }
    return result;
}

/// Why no line could be made.
enum class ShellRcLineError : uint8_t
{
    UnknownShell = 0,
    RelativePath, ///< The binary's path is not absolute, so a shell could not find it from anywhere.
    UnusablePath, ///< The path holds a control character, which no quoting makes safe on one line.
};

namespace detail
{
    /// @return Whether @p c cannot appear in a path written on one rc line.
    [[nodiscard]] constexpr bool isControl(char c) noexcept
    {
        auto const byte = static_cast<unsigned char>(c);
        return byte < 0x20U || byte == 0x7FU;
    }

    /// Quotes @p path in single quotes for @p syntax: POSIX `'` becomes `'\''`; fish `\` becomes `\\`
    /// and `'` becomes `\'`; PowerShell doubles each of `'`, U+2018, U+2019, U+201A and U+201B (all of
    /// which it treats as a single quote), each as itself.
    [[nodiscard]] inline std::string quotePath(std::string_view path, config::RcSyntax syntax)
    {
        auto quoted = std::string { "'" };
        while (!path.empty())
        {
            if (syntax == config::RcSyntax::PowerShell)
            {
                auto const match = std::ranges::find_if(
                    PowerShellQuotes, [&](std::string_view quote) { return path.starts_with(quote); });
                if (match != PowerShellQuotes.end())
                {
                    quoted += *match;
                    quoted += *match;
                    path.remove_prefix(match->size());
                    continue;
                }
            }
            auto const c = path.front();
            path.remove_prefix(1);
            if (c == '\'')
                quoted += syntax == config::RcSyntax::Posix ? "'\\''" : "\\'";
            else if (c == '\\' && syntax == config::RcSyntax::Fish)
                quoted += "\\\\";
            else
                quoted += c;
        }
        quoted += '\'';
        return quoted;
    }
} // namespace detail

/// The one line that loads Contour's integration, guarded so only shells Contour started run it.
/// @param shell The shell the line is for.
/// @param contourBinary The running binary, rooted (config::isRootedPath()).
/// @return The line without a line ending, ending in config::ShellRcMarker; or why it cannot be made.
[[nodiscard]] inline std::expected<std::string, ShellRcLineError> shellRcLine(
    config::ShellKind shell, std::filesystem::path const& contourBinary)
{
    auto const* const row = config::shellRow(shell);
    if (row == nullptr)
        return std::unexpected(ShellRcLineError::UnknownShell);
    if (!config::isRootedPath(contourBinary))
        return std::unexpected(ShellRcLineError::RelativePath);
    auto const path = core::pathToUtf8(contourBinary);
    if (std::ranges::any_of(path, detail::isControl))
        return std::unexpected(ShellRcLineError::UnusablePath);

    auto const quoted = detail::quotePath(path, row->syntax);
    switch (row->syntax)
    {
        case config::RcSyntax::Posix:
            return std::string(row->guard) + R"x(eval "$()x" + quoted + " generate integration shell "
                   + std::string(row->script) + R"x( to -)"; fi )x" + std::string(config::ShellRcMarker);
        case config::RcSyntax::Fish:
            return std::string(row->guard) + quoted + " generate integration shell "
                   + std::string(row->script) + " to - | source; end " + std::string(config::ShellRcMarker);
        case config::RcSyntax::PowerShell:
            return std::string(row->guard) + ". ([scriptblock]::Create((& " + quoted
                   + " generate integration shell " + std::string(row->script) + " to - | Out-String))) } "
                   + std::string(config::ShellRcMarker);
    }
    return std::unexpected(ShellRcLineError::UnknownShell);
}

/// A startup file as the user would say it.
/// @param rc The file.
/// @param home The home directory.
/// @return `~/…` for a path under @p home, else the path itself.
[[nodiscard]] inline std::string displayRcPath(std::filesystem::path const& rc,
                                               std::filesystem::path const& home)
{
    if (!home.empty())
    {
        auto const relative = rc.lexically_relative(home);
        if (!relative.empty() && *relative.begin() != "..")
            return "~/" + relative.generic_string();
    }
    return rc.string();
}

} // namespace contour::window
