// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <orchestrator/core/IssueSources.hpp>
#include <orchestrator/core/Model.hpp>
#include <orchestrator/core/Provider.hpp>

namespace orchestrator
{

/// The kind of item a task works on.
enum class AppliesTo : uint8_t
{
    PullRequest, ///< A change request; spelled `pr`.
    Issue,       ///< An issue; spelled `issue`.
};

/// @param item An item.
/// @return The kind of task it takes: a change request's, or an issue's (forge or Jira).
[[nodiscard]] constexpr AppliesTo appliesToOf(ItemRef const& item) noexcept
{
    return std::holds_alternative<ChangeRequestRef>(item) ? AppliesTo::PullRequest : AppliesTo::Issue;
}

/// One row of the `tasks` table: what the dashboard offers to start on an item. A new task is a new row.
struct TaskDef
{
    std::string id;    ///< Unique among the tasks; an ArgumentKind::Name.
    char key = '\0';   ///< The dashboard key that starts it; unique, and none of the dashboard's own.
    std::string label; ///< What the dashboard calls it.
    AppliesTo appliesTo = AppliesTo::PullRequest; ///< The kind of item it works on.
    std::string model;  ///< The model it runs with by default; one of Config::models().
    std::string prompt; ///< The prompt, with `{url}` standing for the item's URL.
};

/// A program a declared command names that the host runs as its own executable.
struct HostProgram
{
    std::string name; ///< The program as the command's argv names it, such as `contour`.
    std::string path; ///< The executable the host runs for it: absolute.
    [[nodiscard]] bool operator==(HostProgram const&) const = default;
};

/// What the host resolved once (`~/` expanded, symlinks resolved) and passes in host-info; the core never
/// expands a path.
struct HostInfo
{
    std::string home;                   ///< The user's home directory.
    std::string worktreeRoot;           ///< `worktree_root`, resolved.
    std::vector<std::string> repoPaths; ///< Each `repos[].path`, resolved, in the configuration's order.
    int32_t utcOffsetMinutes = 0;       ///< The local time's offset from UTC.
    std::vector<HostProgram> selfPrograms = {}; ///< The programs the host runs as its own executable: what a
                                                ///< command shown to be run by hand names instead.
};

/// One search that lists the issues a repository offers for work.
struct IssueQuery
{
    std::string source; ///< Its issue source's name: `forge` or `jira`.
    std::string text;   ///< The query: a forge search, or a JQL; an ArgumentKind::Text.
    std::string site;   ///< A Jira source's site, without a trailing `/`; empty for the forge.
};

/// One configured repository.
struct Repo
{
    std::string name;                     ///< Unique among the repositories; an ArgumentKind::Name.
    std::string path;                     ///< Its resolved directory, from HostInfo::repoPaths.
    Forge forge = Forge::GitHub;          ///< The forge it lives on.
    std::string remote;                   ///< The git remote that points at the forge; `origin` by default.
    std::optional<std::string> base;      ///< The base branch; absent: the forge's default branch.
    std::vector<IssueQuery> issueQueries; ///< The searches that list the issues offered for work.
};

/// What is wrong with a configuration value. Each problem has one message (see describe()).
enum class ConfigProblem : uint8_t
{
    NotJson,            ///< The configuration is not one complete JSON document.
    UnknownKey,         ///< A key the configuration does not have, such as a misspelt one.
    WrongType,          ///< A value of the wrong JSON type, such as a number where a string belongs.
    Missing,            ///< A required key is absent.
    Empty,              ///< A value that must not be empty is.
    CountMismatch,      ///< The host resolved a different number of repository paths than are configured.
    NotAbsolutePath,    ///< A resolved directory is not an absolute, normalised path.
    EqualRoots,         ///< A repository's directory equals another root.
    InsideWorktreeRoot, ///< A repository lies inside `worktree_root`.
    Duplicate,          ///< The value is already used by another row.
    UnnamedDirectory,   ///< A repository without `name` whose directory's name is not an ArgumentKind::Name.
    DuplicateDirectoryName, ///< A repository without `name` whose directory's name another repository has.
    NotAName,               ///< Not an ArgumentKind::Name.
    NotARef,                ///< Not an ArgumentKind::Ref.
    NotText,                ///< Not an ArgumentKind::Text: too long, or holding a NUL.
    LeadingDash,            ///< Starts with '-', which would read as an option.
    BracketedModel,         ///< A model with a bracketed variant, such as `opus[1m]`.
    UnlistedModel,          ///< A task's model is not one of `models`.
    UnknownForge,           ///< Not a forge any provider stands for.
    UnsupportedSource,      ///< Not an issue source any row stands for.
    NotASite,               ///< Not an `https://` address with a host and no path.
    Unrecorded,             ///< A forge or issue source whose tool's output was never recorded
                            ///< (Verification::Unmeasured).
    UnknownItemKind,        ///< An `applies_to` other than `pr` or `issue`.
    Zero,                   ///< A limit of 0.
    NotADuration,           ///< Not a duration such as `90s`, `2m`, `1h`, `30d` or `0`.
    TooShort,               ///< A duration below its minimum.
    NotOneKey,              ///< A task key that is not one printable character.
    ReservedKey,            ///< A task key the dashboard uses itself.
    MissingUrl,             ///< A prompt without `{url}`.
};

/// Why a configuration was refused.
struct ConfigError
{
    std::string key; ///< Where, such as `repos[0].remote`; empty when the document as a whole is at fault.
    ConfigProblem problem;    ///< What is wrong there.
    std::string subject = {}; ///< What the problem is about, where its message names it, such as `GitLab`.
};

/// The rows a configuration may name: which forges and issue sources exist, and which of them were recorded.
struct SourceTables
{
    std::span<Provider const> providers;          ///< The forges.
    std::span<IssueSourceRow const> issueSources; ///< The issue sources.
};

/// @return The tables the extension ships: providers() and issueSources().
[[nodiscard]] SourceTables sourceTables() noexcept;

/// @param problem A configuration problem.
/// @param sources The rows the configuration was checked against: a problem that lists the forges or the
///        issue sources a configuration may name lists their Recorded ones.
/// @return What it means, as one sentence the user reads after the key.
[[nodiscard]] std::string describe(ConfigProblem problem, SourceTables const& sources);

/// @param error A refused configuration.
/// @param sources The rows it was checked against (Config::parse()'s).
/// @return The message the user reads: the key, then the problem's description.
[[nodiscard]] std::string message(ConfigError const& error, SourceTables const& sources);

/// The most agents running at once, unless `max_agents` says otherwise.
inline constexpr uint32_t DefaultMaxAgents = 4;
/// The most agents running at once in one repository, unless `max_agents_per_repo` says otherwise.
inline constexpr uint32_t DefaultMaxAgentsPerRepo = 2;
/// How often the forges are polled, unless `poll_every` says otherwise.
inline constexpr auto DefaultPollEvery = std::chrono::milliseconds { std::chrono::minutes { 2 } };
/// The shortest `poll_every` accepted.
inline constexpr auto MinPollEvery = std::chrono::milliseconds { std::chrono::seconds { 30 } };
/// How long an agent runs before its status board is looked for, unless `status_board_after` says otherwise;
/// 0 disables the board.
inline constexpr auto DefaultStatusBoardAfter = std::chrono::milliseconds { std::chrono::minutes { 15 } };
/// How long an agent stays done before its run is ended, unless `end_done_after` says otherwise; 0: never,
/// the user ends it.
inline constexpr auto DefaultEndDoneAfter = std::chrono::milliseconds { 0 };
/// How long a finished run is kept, unless `finished_retention` says otherwise.
inline constexpr auto DefaultFinishedRetention = std::chrono::milliseconds { std::chrono::days { 30 } };

/// The orchestrator's configuration, validated in full: a Config that exists is a usable one, and it never
/// changes.
class Config
{
  public:
    /// Reads and validates the configuration.
    /// @param json The `config` subtree, as the host converted it to JSON.
    /// @param host What the host resolved: the paths, one per repository, and the worktree root.
    /// @param sources The forges and issue sources a repository may name; one whose row is Unmeasured is
    ///                refused.
    /// @return The configuration, or the first problem found and where.
    [[nodiscard]] static std::expected<Config, ConfigError> parse(std::string_view json,
                                                                  HostInfo const& host,
                                                                  SourceTables const& sources);

    /// @return The resolved directory under which every agent's worktree lies.
    [[nodiscard]] std::string const& worktreeRoot() const noexcept { return _worktreeRoot; }
    /// @return The models a run may choose, in the configured order.
    [[nodiscard]] std::span<std::string const> models() const noexcept { return _models; }
    /// @return The most agents running at once.
    [[nodiscard]] uint32_t maxAgents() const noexcept { return _maxAgents; }
    /// @return The most agents running at once in one repository.
    [[nodiscard]] uint32_t maxAgentsPerRepo() const noexcept { return _maxAgentsPerRepo; }
    /// @return How often the forges are polled.
    [[nodiscard]] std::chrono::milliseconds pollEvery() const noexcept { return _pollEvery; }
    /// @return How long an agent runs before its status board is looked for; 0: never.
    [[nodiscard]] std::chrono::milliseconds statusBoardAfter() const noexcept { return _statusBoardAfter; }
    /// @return How long an agent stays done before its run is ended; 0: never.
    [[nodiscard]] std::chrono::milliseconds endDoneAfter() const noexcept { return _endDoneAfter; }
    /// @return How long a finished run is kept.
    [[nodiscard]] std::chrono::milliseconds finishedRetention() const noexcept { return _finishedRetention; }
    /// @return The repositories, in the configured order.
    [[nodiscard]] std::span<Repo const> repos() const noexcept { return _repos; }
    /// @return The tasks, in the configured order.
    [[nodiscard]] std::span<TaskDef const> tasks() const noexcept { return _tasks; }
    /// @return The forges and issue sources it was read with: the rows its repositories are served by.
    [[nodiscard]] SourceTables const& sources() const noexcept { return _sources; }

    /// @param name A repository name.
    /// @return The repository of that name, or nullptr.
    [[nodiscard]] Repo const* findRepo(std::string_view name) const noexcept;
    /// @param id A task id.
    /// @return The task of that id, or nullptr.
    [[nodiscard]] TaskDef const* findTask(std::string_view id) const noexcept;
    /// @param item A kind of item.
    /// @return The first task in the table that applies to @p item, or nullptr: what Enter offers.
    [[nodiscard]] TaskDef const* firstTaskFor(AppliesTo item) const noexcept;

  private:
    /// Everything a Config holds, in one aggregate, so that parse() builds it field by field.
    struct Values
    {
        std::string worktreeRoot = {};
        std::vector<std::string> models = {};
        uint32_t maxAgents = DefaultMaxAgents;
        uint32_t maxAgentsPerRepo = DefaultMaxAgentsPerRepo;
        std::chrono::milliseconds pollEvery = DefaultPollEvery;
        std::chrono::milliseconds statusBoardAfter = DefaultStatusBoardAfter;
        std::chrono::milliseconds endDoneAfter = DefaultEndDoneAfter;
        std::chrono::milliseconds finishedRetention = DefaultFinishedRetention;
        std::vector<Repo> repos = {};
        std::vector<TaskDef> tasks = {};
        SourceTables sources = {};
    };

    explicit Config(Values values) noexcept;

    std::string _worktreeRoot;
    std::vector<std::string> _models;
    uint32_t _maxAgents;
    uint32_t _maxAgentsPerRepo;
    std::chrono::milliseconds _pollEvery;
    std::chrono::milliseconds _statusBoardAfter;
    std::chrono::milliseconds _endDoneAfter;
    std::chrono::milliseconds _finishedRetention;
    std::vector<Repo> _repos;
    std::vector<TaskDef> _tasks;
    SourceTables _sources;
};

/// Reads a duration: a number of 1 to 9 digits followed by one unit, `s`, `m`, `h` or `d`; or a bare `0`.
/// @param text Such as "90s", "2m", "1h", "30d" or "0".
/// @return The duration, or nothing if @p text is not one.
[[nodiscard]] std::optional<std::chrono::milliseconds> parseDuration(std::string_view text) noexcept;

} // namespace orchestrator
