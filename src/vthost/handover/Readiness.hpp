// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The readiness check a live upgrade runs before it stops anything: the target binary is asked, in a query
/// mode (`contour daemon handover-info`), which release it is and which formats it reads.

#include <vtpty/InheritedPty.hpp>

#include <core/async/IExecutor.hpp>
#include <core/async/Task.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <vthost/handover/UpgradeError.hpp>

namespace vtpty
{
class CommandCapture;
}

namespace vthost::handover
{

/// Whether a binary can adopt the pseudoconsoles a daemon hands over (Windows).
enum class PseudoconsoleAdoption : uint8_t
{
    Unable = 0, ///< It cannot: its bundled conpty.dll does not load, or lacks the entry point.
    Able,       ///< It can.
};

/// What a binary says of itself when asked whether it can take a handover over.
struct ReadinessReport
{
    std::string release;                                   ///< The release it reports.
    std::pair<uint32_t, uint32_t> handoverFormats {};      ///< The handover formats it reads: oldest, newest.
    std::pair<uint32_t, uint32_t> terminalStateFormats {}; ///< The terminal state formats it reads.
    /// Why it cannot run as the daemon's configuration says (ReadinessQuery::configFile and ::profile): the
    /// file does not load, or no longer names the profile; nullopt when it can, or was asked about none.
    std::optional<std::string> configProblem {};
    /// Whether it adopts pseudoconsoles; nullopt where it does not say (a release that predates the field, or
    /// a platform without pseudoconsoles), which never refuses an upgrade.
    std::optional<PseudoconsoleAdoption> pseudoconsoleAdoption {};

    bool operator==(ReadinessReport const&) const = default;
};

namespace detail
{
    /// A ReadinessReport as its JSON spells it, each range an array of two, oldest first. Declared here only
    /// because glaze reflects a type by its members' names, which a type of one translation unit's own does
    /// not give it.
    struct ReadinessJson
    {
        std::string release;                             ///< ReadinessReport::release.
        std::array<uint32_t, 2> handoverFormats {};      ///< ReadinessReport::handoverFormats.
        std::array<uint32_t, 2> terminalStateFormats {}; ///< ReadinessReport::terminalStateFormats.
        std::optional<std::string> configProblem {};     ///< ReadinessReport::configProblem; absent if none.
        /// ReadinessReport::pseudoconsoleAdoption, a wire boundary and so a bool; absent if unknown.
        std::optional<bool> adoptsPseudoconsoles {};
    };
} // namespace detail

/// @param adoptability Whether a process can adopt pseudoconsoles (vtpty::pseudoconsoleAdoptability()).
/// @return It as a readiness report says it: nothing where the platform has no pseudoconsoles.
[[nodiscard]] constexpr std::optional<PseudoconsoleAdoption> reportedAdoption(
    vtpty::PseudoconsoleAdoptability adoptability) noexcept
{
    switch (adoptability)
    {
        case vtpty::PseudoconsoleAdoptability::NotApplicable: return std::nullopt;
        case vtpty::PseudoconsoleAdoptability::Unable: return PseudoconsoleAdoption::Unable;
        case vtpty::PseudoconsoleAdoptability::Able: return PseudoconsoleAdoption::Able;
    }
    return std::nullopt;
}

/// @param release The release this build reports.
/// @param pseudoconsoles Whether this build adopts pseudoconsoles (reportedAdoption()); nullopt where it says
///        nothing of them.
/// @return What this build says of itself: the formats it reads (HandoverImage.hpp, StateCodec.hpp), and
///         @p pseudoconsoles.
[[nodiscard]] ReadinessReport ownReadiness(std::string release,
                                           std::optional<PseudoconsoleAdoption> pseudoconsoles);

/// @param report A report.
/// @return It as one line of strict JSON, no line break:
///         `{"release":"…","handoverFormats":[min,max],"terminalStateFormats":[min,max]}`, with
///         `"configProblem":"…"` and `"adoptsPseudoconsoles":true|false` added where the report has them.
[[nodiscard]] std::string encodeReadiness(ReadinessReport const& report);

/// Reads what encodeReadiness() wrote, of this or a later release: a key it does not know is skipped, one it
/// needs is required.
/// @param text The JSON.
/// @return The report; nullopt for anything else, such as the usage error of a release without the query.
[[nodiscard]] std::optional<ReadinessReport> decodeReadiness(std::string_view text);

/// @param target What the target binary reports.
/// @return Nothing when it reads both formats this build writes; FormatUnreadable otherwise.
[[nodiscard]] std::expected<void, UpgradeRefusal> checkReadable(ReadinessReport const& target);

/// @param target What the target binary reports.
/// @return Nothing unless it reports it cannot adopt pseudoconsoles, then TargetCannotAdoptTerminals. For a
///         daemon that hosts one: a daemon that hosts none has nothing to hand over to it.
[[nodiscard]] std::expected<void, UpgradeRefusal> checkAdoptsPseudoconsoles(ReadinessReport const& target);

/// @param target What the target binary reports.
/// @return Nothing when it can run as the daemon's configuration says; ConfigUnreadable otherwise.
[[nodiscard]] std::expected<void, UpgradeRefusal> checkConfigured(ReadinessReport const& target);

/// Where the configuration the running daemon serves on came from.
enum class RunningConfig : uint8_t
{
    BuiltInDefaults = 0, ///< Its file did not load: it serves on built-in defaults, as the target would.
    LoadedFromFile,      ///< It loaded the file, so a target that cannot has to be refused.
};

/// What a readiness probe asks a binary about.
struct ReadinessQuery
{
    std::filesystem::path executable; ///< The binary to ask.
    /// The configuration file the daemon runs on, which the binary loads to show it can; empty for none.
    std::filesystem::path configFile {};
    /// The profile the daemon's sessions run, which the binary resolves in it; empty for the default one.
    std::string profile {};
};

/// Asks a binary whether it can take a handover over.
///
/// An interface, because the native probe runs a program: the orchestrator is tested over a fake. A
/// coroutine, because the native probe waits seconds on that program, which it does off the loop it is
/// awaited on.
class ReadinessProbe
{
  public:
    ReadinessProbe() = default;
    ReadinessProbe(ReadinessProbe const&) = delete;
    ReadinessProbe(ReadinessProbe&&) = delete;
    ReadinessProbe& operator=(ReadinessProbe const&) = delete;
    ReadinessProbe& operator=(ReadinessProbe&&) = delete;
    virtual ~ReadinessProbe() = default;

    /// @param query The binary to ask, and the configuration it is to load.
    /// @return What it says; TargetUnresponsive when it could not be run or did not answer in time,
    ///         FormatUnreadable when its answer is no readiness report (a release that predates the query).
    ///         Completes on the executor it was awaited on.
    [[nodiscard]] virtual core::async::Task<std::expected<ReadinessReport, UpgradeRefusal>> probe(
        ReadinessQuery query) = 0;
};

/// How long the native probe waits for the target's answer.
inline constexpr auto DefaultReadinessTimeout = std::chrono::milliseconds { 5000 };

/// @param query The binary to ask, and the configuration it is to load.
/// @return The command the native probe runs: the binary's query mode, given the configuration file and
///         profile when named, in the daemon's own environment, unchanged: the target inherits that
///         environment, so the probe asks the target in the world it will run in. A shell command on POSIX,
///         which execs the binary; on Windows the binary's own command line, run with no shell between
///         (vtpty::ProgramCommandCapture): under cmd.exe a release build, a GUI-subsystem program that
///         attaches to its parent's console, would write to that console instead.
[[nodiscard]] std::string readinessCommand(ReadinessQuery const& query);

/// Asks a binary by running its query mode (readinessCommand()) through a command capture, on a worker, so
/// that the loop it is awaited on serves on meanwhile; it completes back on that loop.
class CommandReadinessProbe final: public ReadinessProbe
{
  public:
    /// @param capture What runs the command (not owned; outlives this).
    /// @param worker Where the command is run and waited for (not owned; outlives this).
    /// @param timeout How long the answer is waited for; the command is killed after it.
    CommandReadinessProbe(vtpty::CommandCapture& capture,
                          core::async::IExecutor& worker,
                          std::chrono::milliseconds timeout = DefaultReadinessTimeout) noexcept:
        _capture { capture }, _worker { worker }, _timeout { timeout }
    {
    }

    [[nodiscard]] core::async::Task<std::expected<ReadinessReport, UpgradeRefusal>> probe(
        ReadinessQuery query) override;

  private:
    /// Runs the command and reads its answer, on whatever thread calls it.
    [[nodiscard]] std::expected<ReadinessReport, UpgradeRefusal> ask(ReadinessQuery const& query) const;

    vtpty::CommandCapture& _capture;
    core::async::IExecutor& _worker;
    std::chrono::milliseconds _timeout;
};

} // namespace vthost::handover
