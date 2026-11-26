// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/core/Config.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <iterator>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <glaze/glaze.hpp>
#include <orchestrator/core/ArgumentKinds.hpp>
#include <orchestrator/core/ConfigJson.hpp>
#include <orchestrator/core/DashboardKeys.hpp>
#include <orchestrator/core/HostPaths.hpp>
#include <orchestrator/core/IssueSources.hpp>
#include <orchestrator/core/Jira.hpp>
#include <orchestrator/core/Json.hpp>
#include <orchestrator/core/Model.hpp>
#include <orchestrator/core/NavKeys.hpp>
#include <orchestrator/core/Overloaded.hpp>
#include <orchestrator/core/Prompt.hpp>
#include <orchestrator/core/Provider.hpp>
#include <orchestrator/core/QuickReplies.hpp>
#include <orchestrator/core/WorkGroups.hpp>

using namespace std::string_view_literals;

/// The configuration's snake_case keys.
template <>
struct glz::meta<orchestrator::config::detail::JsonConfig>
{
    using T = orchestrator::config::detail::JsonConfig;
    // Glaze finds the table by this spelling, as the standard library finds `value_type`.
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr auto value = glz::object("worktree_root",
                                              &T::worktreeRoot,
                                              "models",
                                              &T::models,
                                              "max_agents",
                                              &T::maxAgents,
                                              "max_agents_per_repo",
                                              &T::maxAgentsPerRepo,
                                              "poll_every",
                                              &T::pollEvery,
                                              "status_board_after",
                                              &T::statusBoardAfter,
                                              "status_board",
                                              &T::statusBoard,
                                              "end_done_after",
                                              &T::endDoneAfter,
                                              "finished_retention",
                                              &T::finishedRetention,
                                              "repos",
                                              &T::repos,
                                              "workers",
                                              &T::workers,
                                              "tasks",
                                              &T::tasks,
                                              "plugins",
                                              &T::plugins,
                                              "permissions",
                                              &T::permissions,
                                              "permission_profiles",
                                              &T::permissionProfiles,
                                              "quick_replies",
                                              &T::quickReplies,
                                              "keys",
                                              &T::keys,
                                              "work",
                                              &T::work,
                                              "automation",
                                              &T::automation);
};

/// The `automation` map's snake_case names.
template <>
struct glz::meta<orchestrator::config::detail::JsonAutomation>
{
    using T = orchestrator::config::detail::JsonAutomation;
    // Glaze finds the table by this spelling, as the standard library finds `value_type`.
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr auto value =
        glz::object("enabled", &T::enabled, "max_agents", &T::maxAgents, "rules", &T::rules);
};

/// The `work` map's snake_case names.
template <>
struct glz::meta<orchestrator::config::detail::JsonWork>
{
    using T = orchestrator::config::detail::JsonWork;
    // Glaze finds the table by this spelling, as the standard library finds `value_type`.
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr auto value = glz::object("folded", &T::folded, "fold_empty", &T::foldEmpty);
};

/// The `keys` map's snake_case names.
template <>
struct glz::meta<orchestrator::config::detail::JsonKeys>
{
    using T = orchestrator::config::detail::JsonKeys;
    // Glaze finds the table by this spelling, as the standard library finds `value_type`.
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr auto value = glz::object("vim",
                                              &T::vim,
                                              "move_up",
                                              &T::moveUp,
                                              "move_down",
                                              &T::moveDown,
                                              "first",
                                              &T::first,
                                              "last",
                                              &T::last,
                                              "half_page_up",
                                              &T::halfPageUp,
                                              "half_page_down",
                                              &T::halfPageDown,
                                              "previous_value",
                                              &T::previousValue,
                                              "next_value",
                                              &T::nextValue,
                                              "leave_field",
                                              &T::leaveField,
                                              "enter_field",
                                              &T::enterField,
                                              "close",
                                              &T::close);
};

/// A task's keys: `applies_to` is no member name.
template <>
struct glz::meta<orchestrator::config::detail::JsonTask>
{
    using T = orchestrator::config::detail::JsonTask;
    // Glaze finds the table by this spelling, as the standard library finds `value_type`.
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr auto value = glz::object("id",
                                              &T::id,
                                              "key",
                                              &T::key,
                                              "label",
                                              &T::label,
                                              "applies_to",
                                              &T::appliesTo,
                                              "model",
                                              &T::model,
                                              "prompt",
                                              &T::prompt,
                                              "end_done_after",
                                              &T::endDoneAfter,
                                              "permissions",
                                              &T::permissions,
                                              "worker",
                                              &T::worker,
                                              "role",
                                              &T::role);
};

/// A worker's keys: `env_set` is no member name.
template <>
struct glz::meta<orchestrator::config::detail::JsonWorker>
{
    using T = orchestrator::config::detail::JsonWorker;
    // Glaze finds the table by this spelling, as the standard library finds `value_type`.
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr auto value = glz::object("id", &T::id, "agent", &T::agent, "env_set", &T::envSet);
};

/// An issue source's keys: `in_progress_jql` is no member name.
template <>
struct glz::meta<orchestrator::config::detail::JsonIssueSource>
{
    using T = orchestrator::config::detail::JsonIssueSource;
    // Glaze finds the table by this spelling, as the standard library finds `value_type`.
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr auto value = glz::object("source",
                                              &T::source,
                                              "query",
                                              &T::query,
                                              "jql",
                                              &T::jql,
                                              "in_progress_jql",
                                              &T::inProgressJql,
                                              "site",
                                              &T::site);
};

namespace orchestrator
{

namespace
{
    using config::detail::JsonAutomation;
    using config::detail::JsonAutomationRule;
    using config::detail::JsonConfig;
    using config::detail::JsonDuration;
    using config::detail::JsonIssueSource;
    using config::detail::JsonKey;
    using config::detail::JsonKeys;
    using config::detail::JsonPermissionProfile;
    using config::detail::JsonPluginRequirement;
    using config::detail::JsonQuickReply;
    using config::detail::JsonRepo;
    using config::detail::JsonTask;
    using config::detail::JsonWork;
    using config::detail::JsonWorker;

    template <typename T>
    using Checked = std::expected<T, ConfigError>;

    /// The input is a string_view: nothing terminates it. An unknown key is refused, so a misspelt one does
    /// not silently keep its default.
    constexpr auto ReadOptions = glz::opts { .null_terminated = false };

    /// One row of a table that names what a configured string means.
    template <typename Meaning>
    struct Spelling
    {
        std::string_view text;
        Meaning meaning;
    };

    constexpr auto AppliesToSpellings = std::array {
        Spelling { .text = "pr"sv, .meaning = AppliesTo::PullRequest },
        Spelling { .text = "issue"sv, .meaning = AppliesTo::Issue },
        Spelling { .text = "topic"sv, .meaning = AppliesTo::Topic },
    };

    /// @return The meaning of @p text in @p table, or nothing if no row spells it.
    template <typename Meaning, std::size_t N>
    constexpr std::optional<Meaning> meaningOf(std::array<Spelling<Meaning>, N> const& table,
                                               std::string_view text) noexcept
    {
        auto const row = std::ranges::find(table, text, &Spelling<Meaning>::text);
        return row != table.end() ? std::optional { row->meaning } : std::nullopt;
    }

    /// @return Whether a task key of @p character would be one the dashboard binds itself.
    [[nodiscard]] bool isDashboardKey(char character) noexcept
    {
        return character != '\0'
               && std::ranges::contains(dashboardKeys(), character, &DashboardKey::character);
    }

    /// The accepted values a problem's message lists, taken from the table that decides them.
    enum class Listing : uint8_t
    {
        None,
        Forges,          ///< ForgeSpellings whose provider row is Recorded.
        IssueSources,    ///< The issueSources() that are Recorded.
        ItemKinds,       ///< AppliesToSpellings.
        ReservedKeys,    ///< The dashboardKeys() that type a character.
        PermissionModes, ///< PermissionModeNames.
        BuiltinProfiles, ///< builtinProfiles().
        PaneKeys,        ///< paneKeyNames().
        AfterReplies,    ///< afterReplyNames().
        TaskRoles,       ///< TaskRoleRows.
        KeyNames,        ///< SpecialKeyNames.
        WorkGroups,      ///< WorkGroupRows' configNames.
        Conditions,      ///< AutomationConditionRows' names.
    };

    /// What each problem means; one row per ConfigProblem, in its order.
    struct ProblemText
    {
        ConfigProblem problem;
        std::string_view text;
        Listing listing = Listing::None;
        std::string_view afterSubject = {}; ///< Follows the error's subject, where it names one.
    };

    constexpr auto ProblemTexts = std::array {
        ProblemText { .problem = ConfigProblem::NotJson,
                      .text = "the configuration is not one complete JSON document" },
        ProblemText { .problem = ConfigProblem::UnknownKey, .text = "is not a configuration key" },
        ProblemText { .problem = ConfigProblem::WrongType, .text = "has a value of the wrong type" },
        ProblemText { .problem = ConfigProblem::Missing, .text = "is required" },
        ProblemText { .problem = ConfigProblem::Empty, .text = "must not be empty" },
        ProblemText { .problem = ConfigProblem::CountMismatch,
                      .text = "the host resolved a different number of repository paths" },
        ProblemText { .problem = ConfigProblem::NotAbsolutePath,
                      .text = "does not resolve to an absolute, normalised path" },
        ProblemText { .problem = ConfigProblem::EqualRoots,
                      .text = "is the same directory as worktree_root or another repository" },
        ProblemText { .problem = ConfigProblem::InsideWorktreeRoot, .text = "lies inside worktree_root" },
        ProblemText { .problem = ConfigProblem::Duplicate, .text = "is already used by another entry" },
        ProblemText { .problem = ConfigProblem::UnnamedDirectory,
                      .text = "its directory's name is not a valid repository name; set `name`" },
        ProblemText { .problem = ConfigProblem::DuplicateDirectoryName,
                      .text = "its directory's name is already another repository's name; set `name`" },
        ProblemText { .problem = ConfigProblem::NotAName,
                      .text =
                          "must start with a letter or digit and continue with letters, digits, '.', '_' or "
                          "'-', at most 64 characters" },
        ProblemText { .problem = ConfigProblem::NotARef,
                      .text =
                          "is not a branch name: letters, digits and . _ / @ # + -, without '..', '//' or "
                          "'@{'" },
        ProblemText { .problem = ConfigProblem::NotText,
                      .text = "is longer than 8 KiB or holds a NUL character" },
        ProblemText { .problem = ConfigProblem::LeadingDash,
                      .text =
                          "must not start with '-', which would read as an option; lead with another term" },
        ProblemText { .problem = ConfigProblem::BracketedModel,
                      .text = "bracketed model variants are not supported; use a plain alias or id" },
        ProblemText { .problem = ConfigProblem::UnlistedModel,
                      .text = "is not one of the configured models" },
        ProblemText { .problem = ConfigProblem::UnknownForge,
                      .text = "is not a supported forge",
                      .listing = Listing::Forges },
        ProblemText { .problem = ConfigProblem::UnsupportedSource,
                      .text = "is not a supported issue source",
                      .listing = Listing::IssueSources },
        ProblemText { .problem = ConfigProblem::NotASite,
                      .text = "must be an https:// address with a host and no path, such as "
                              "https://example.atlassian.net" },
        ProblemText { .problem = ConfigProblem::Unrecorded,
                      .text = "is not yet supported",
                      .afterSubject = " has no recorded fixture" },
        ProblemText { .problem = ConfigProblem::UnknownItemKind,
                      .text = "must be one of",
                      .listing = Listing::ItemKinds },
        ProblemText { .problem = ConfigProblem::Zero, .text = "must be at least 1" },
        ProblemText { .problem = ConfigProblem::NotADuration,
                      .text =
                          "is not a duration: a number and one unit (s, m, h, d), such as 90s, 2m or 30d" },
        ProblemText { .problem = ConfigProblem::TooShort, .text = "must be at least 30s" },
        ProblemText { .problem = ConfigProblem::NotOneKey, .text = "must be one printable character" },
        ProblemText { .problem = ConfigProblem::ReservedKey,
                      .text = "is one of the dashboard's own keys",
                      .listing = Listing::ReservedKeys },
        ProblemText { .problem = ConfigProblem::MissingUrl, .text = "must contain {url} or {key}" },
        ProblemText { .problem = ConfigProblem::KeyInTopic,
                      .text = "may not use {key}: a topic has no item, so no key" },
        ProblemText { .problem = ConfigProblem::NotPluginId,
                      .text = "must be <plugin>@<marketplace>, such as contour-workflows@contour-terminal" },
        ProblemText { .problem = ConfigProblem::UnknownProfile,
                      .text = "names no permission profile; the built-in ones are",
                      .listing = Listing::BuiltinProfiles },
        ProblemText { .problem = ConfigProblem::ProfileCycle,
                      .text = "makes a permission profile extend itself" },
        ProblemText { .problem = ConfigProblem::NotPermissionMode,
                      .text = "must be one of",
                      .listing = Listing::PermissionModes },
        ProblemText { .problem = ConfigProblem::BypassRefused,
                      .text = "is refused: bypassPermissions would let an agent do anything without asking" },
        ProblemText { .problem = ConfigProblem::NotPermissionRule,
                      .text =
                          "must be a tool, or a tool and its specifier, such as Read or Bash(git push:*)" },
        ProblemText { .problem = ConfigProblem::SettingsTooLarge,
                      .text = "holds more rules than one --settings argument can carry" },
        ProblemText { .problem = ConfigProblem::NotAReplyKey, .text = "must be a digit from 1 to 9" },
        ProblemText {
            .problem = ConfigProblem::NotAPaneKey, .text = "must be one of", .listing = Listing::PaneKeys },
        ProblemText { .problem = ConfigProblem::UnknownThen,
                      .text = "must be one of",
                      .listing = Listing::AfterReplies },
        ProblemText { .problem = ConfigProblem::LabelTooLong, .text = "is longer than 40 bytes" },
        ProblemText { .problem = ConfigProblem::UnknownAgent, .text = "is not an agent: claude or copilot" },
        ProblemText { .problem = ConfigProblem::UnknownWorker,
                      .text = "names no worker; the first worker runs it" },
        ProblemText { .problem = ConfigProblem::ProfileOnCopilot,
                      .text = "is a permission profile, which a Copilot worker does not take" },
        ProblemText {
            .problem = ConfigProblem::UnknownRole, .text = "must be one of", .listing = Listing::TaskRoles },
        ProblemText {
            .problem = ConfigProblem::NotAKeyName,
            .text =
                "is not a key: one printable character, ^ and a lower-case letter (not h, i, j or m), two "
                "characters such as gg, or one of",
            .listing = Listing::KeyNames },
        ProblemText { .problem = ConfigProblem::KeyTaken, .text = "is already bound" },
        ProblemText { .problem = ConfigProblem::TypesText,
                      .text =
                          "would type into a text field instead of leaving it; use esc, ^ and a letter, or "
                          "another special key" },
        ProblemText { .problem = ConfigProblem::UnknownGroup,
                      .text = "must be one of",
                      .listing = Listing::WorkGroups },
        ProblemText { .problem = ConfigProblem::UnknownCondition,
                      .text = "is no condition; the conditions are",
                      .listing = Listing::Conditions },
        ProblemText { .problem = ConfigProblem::UnknownRuleTask,
                      .text = "is no configured task a rule can start" },
        ProblemText { .problem = ConfigProblem::RuleTaskMismatch,
                      .text = "does not apply to the items this condition matches" },
        ProblemText { .problem = ConfigProblem::UnknownRuleWorker, .text = "names no worker" },
        ProblemText { .problem = ConfigProblem::RuleWorkerTakesNoProfile,
                      .text = "takes no permission profile, and the task has one" },
    };

    static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, ProblemTexts.size()),
                                      [](std::size_t i) {
                                          return static_cast<std::size_t>(ProblemTexts[i].problem) == i;
                                      }),
                  "ProblemTexts has one row per ConfigProblem, in its order");
    static_assert(ProblemTexts.size()
                      == static_cast<std::size_t>(ConfigProblem::RuleWorkerTakesNoProfile) + 1,
                  "ProblemTexts covers every ConfigProblem");

    /// @return The values @p listing names, as the user writes them, of the rows @p sources holds.
    [[nodiscard]] std::vector<std::string_view> listed(Listing listing, SourceTables const& sources)
    {
        auto values = std::vector<std::string_view> {};
        switch (listing)
        {
            case Listing::None: break;
            case Listing::Forges:
                for (auto const& row: ForgeSpellings)
                    if (auto const* const provider = findProvider(sources.providers, row.forge);
                        provider != nullptr && provider->verification == Verification::Recorded)
                        values.push_back(row.name);
                break;
            case Listing::IssueSources:
                for (auto const& row: sources.issueSources)
                    if (row.verification == Verification::Recorded)
                        values.push_back(row.name);
                break;
            case Listing::ItemKinds:
                for (auto const& row: AppliesToSpellings)
                    values.push_back(row.text);
                break;
            case Listing::ReservedKeys:
                for (auto const& row: dashboardKeys())
                    if (row.character != '\0')
                        values.push_back(row.name);
                break;
            case Listing::PermissionModes:
                for (auto const& row: PermissionModeNames)
                    values.push_back(row.word);
                break;
            case Listing::BuiltinProfiles:
                for (auto const& row: builtinProfiles())
                    values.push_back(row.name);
                break;
            case Listing::PaneKeys: std::ranges::copy(paneKeyNames(), std::back_inserter(values)); break;
            case Listing::AfterReplies:
                std::ranges::copy(afterReplyNames(), std::back_inserter(values));
                break;
            case Listing::TaskRoles:
                for (auto const& row: TaskRoleRows)
                    values.push_back(row.spelling);
                break;
            case Listing::KeyNames: std::ranges::copy(SpecialKeyNames, std::back_inserter(values)); break;
            case Listing::WorkGroups:
                for (auto const& row: WorkGroupRows)
                    values.push_back(row.configName);
                break;
            case Listing::Conditions:
                for (auto const& row: AutomationConditionRows)
                    values.push_back(row.name);
                break;
        }
        return values;
    }

    /// The remote a repository uses unless it names another.
    constexpr auto DefaultRemote = "origin"sv;
    /// The models a run may choose unless `models` names others.
    constexpr auto DefaultModels = std::array { "opus"sv, "sonnet"sv, "haiku"sv };

    /// One duration unit.
    struct DurationUnit
    {
        char suffix;
        std::chrono::milliseconds length;
    };

    constexpr auto DurationUnits = std::array {
        DurationUnit { .suffix = 's', .length = std::chrono::seconds { 1 } },
        DurationUnit { .suffix = 'm', .length = std::chrono::minutes { 1 } },
        DurationUnit { .suffix = 'h', .length = std::chrono::hours { 1 } },
        DurationUnit { .suffix = 'd', .length = std::chrono::days { 1 } },
    };

    /// The most digits a duration has: 999999999d is still far from overflowing milliseconds.
    constexpr std::size_t MaxDurationDigits = 9;

    [[nodiscard]] std::unexpected<ConfigError> fail(std::string key, ConfigProblem problem)
    {
        return std::unexpected(ConfigError { .key = std::move(key), .problem = problem });
    }

    /// The JSON types a configuration value may have; null is accepted everywhere and means absent.
    enum class JsonType : uint8_t
    {
        String,   ///< A string.
        Count,    ///< A non-negative integer that fits 32 bits.
        Duration, ///< A string, or a non-negative integer (only 0 is accepted later).
        Key,      ///< A string, or a non-negative integer: a digit.
        Strings,  ///< An array of strings.
        Objects,  ///< An array of objects of Field::object's kind.
        Bool,     ///< `true` or `false`.
        Object,   ///< One object of Field::object's kind.
        KeyList,  ///< An array of keys: strings, or integers 0 to 9.
    };

    /// The objects the configuration nests.
    enum class ObjectKind : uint8_t
    {
        Config,
        Repo,
        IssueSource,
        Task,
        Plugin,
        PermissionProfile,
        QuickReply,
        Worker,
        Keys,
        Work,
        Automation,
        AutomationRule,
    };

    /// One key an object may have, and the type of its value.
    struct Field
    {
        std::string_view key;
        JsonType type;
        ObjectKind object =
            ObjectKind::Config; ///< For Objects: the kind of its elements; for Object, its own.
    };

    constexpr auto ConfigFields = std::array {
        Field { .key = "worktree_root", .type = JsonType::String },
        Field { .key = "models", .type = JsonType::Strings },
        Field { .key = "max_agents", .type = JsonType::Count },
        Field { .key = "max_agents_per_repo", .type = JsonType::Count },
        Field { .key = "poll_every", .type = JsonType::Duration },
        Field { .key = "status_board_after", .type = JsonType::Duration },
        Field { .key = "status_board", .type = JsonType::String },
        Field { .key = "end_done_after", .type = JsonType::Duration },
        Field { .key = "finished_retention", .type = JsonType::Duration },
        Field { .key = "repos", .type = JsonType::Objects, .object = ObjectKind::Repo },
        Field { .key = "workers", .type = JsonType::Objects, .object = ObjectKind::Worker },
        Field { .key = "tasks", .type = JsonType::Objects, .object = ObjectKind::Task },
        Field { .key = "plugins", .type = JsonType::Objects, .object = ObjectKind::Plugin },
        Field { .key = "permissions", .type = JsonType::String },
        Field { .key = "permission_profiles",
                .type = JsonType::Objects,
                .object = ObjectKind::PermissionProfile },
        Field { .key = "quick_replies", .type = JsonType::Objects, .object = ObjectKind::QuickReply },
        Field { .key = "keys", .type = JsonType::Object, .object = ObjectKind::Keys },
        Field { .key = "work", .type = JsonType::Object, .object = ObjectKind::Work },
        Field { .key = "automation", .type = JsonType::Object, .object = ObjectKind::Automation },
    };

    constexpr auto RepoFields = std::array {
        Field { .key = "name", .type = JsonType::String },
        Field { .key = "path", .type = JsonType::String },
        Field { .key = "forge", .type = JsonType::String },
        Field { .key = "remote", .type = JsonType::String },
        Field { .key = "base", .type = JsonType::String },
        Field { .key = "issues", .type = JsonType::Objects, .object = ObjectKind::IssueSource },
    };

    constexpr auto IssueSourceFields = std::array {
        Field { .key = "source", .type = JsonType::String },
        Field { .key = "query", .type = JsonType::String },
        Field { .key = "jql", .type = JsonType::String },
        Field { .key = "in_progress_jql", .type = JsonType::String },
        Field { .key = "site", .type = JsonType::String },
    };

    constexpr auto TaskFields = std::array {
        Field { .key = "id", .type = JsonType::String },
        Field { .key = "key", .type = JsonType::String },
        Field { .key = "label", .type = JsonType::String },
        Field { .key = "applies_to", .type = JsonType::String },
        Field { .key = "model", .type = JsonType::String },
        Field { .key = "prompt", .type = JsonType::String },
        Field { .key = "end_done_after", .type = JsonType::Duration },
        Field { .key = "permissions", .type = JsonType::String },
        Field { .key = "worker", .type = JsonType::String },
        Field { .key = "role", .type = JsonType::String },
    };

    constexpr auto WorkerFields = std::array {
        Field { .key = "id", .type = JsonType::String },
        Field { .key = "agent", .type = JsonType::String },
        Field { .key = "env_set", .type = JsonType::String },
    };

    constexpr auto PluginFields = std::array {
        Field { .key = "id", .type = JsonType::String },
        Field { .key = "marketplace", .type = JsonType::String },
    };

    constexpr auto PermissionProfileFields = std::array {
        Field { .key = "name", .type = JsonType::String },
        Field { .key = "extends", .type = JsonType::String },
        Field { .key = "mode", .type = JsonType::String },
        Field { .key = "allow", .type = JsonType::Strings },
        Field { .key = "deny", .type = JsonType::Strings },
    };

    constexpr auto QuickReplyFields = std::array {
        Field { .key = "key", .type = JsonType::Key },
        Field { .key = "label", .type = JsonType::String },
        Field { .key = "press", .type = JsonType::Key },
        Field { .key = "then", .type = JsonType::String },
    };

    /// `vim`, then one KeyList per navigation action, named as navActionRows() names them.
    constexpr auto KeysFields = std::array {
        Field { .key = "vim", .type = JsonType::Bool },
        Field { .key = "move_up", .type = JsonType::KeyList },
        Field { .key = "move_down", .type = JsonType::KeyList },
        Field { .key = "first", .type = JsonType::KeyList },
        Field { .key = "last", .type = JsonType::KeyList },
        Field { .key = "half_page_up", .type = JsonType::KeyList },
        Field { .key = "half_page_down", .type = JsonType::KeyList },
        Field { .key = "previous_value", .type = JsonType::KeyList },
        Field { .key = "next_value", .type = JsonType::KeyList },
        Field { .key = "leave_field", .type = JsonType::KeyList },
        Field { .key = "enter_field", .type = JsonType::KeyList },
        Field { .key = "close", .type = JsonType::KeyList },
    };

    /// Each action's list in JsonKeys, in navActionRows() order.
    constexpr auto KeyListMembers = std::array {
        &JsonKeys::moveUp,     &JsonKeys::moveDown,     &JsonKeys::first,         &JsonKeys::last,
        &JsonKeys::halfPageUp, &JsonKeys::halfPageDown, &JsonKeys::previousValue, &JsonKeys::nextValue,
        &JsonKeys::leaveField, &JsonKeys::enterField,   &JsonKeys::close,
    };
    static_assert(KeyListMembers.size() == NavActionCount, "KeyListMembers has one member per NavAction");
    static_assert(KeysFields.size() == NavActionCount + 1, "KeysFields is vim and one list per NavAction");

    constexpr auto WorkFields = std::array {
        Field { .key = "folded", .type = JsonType::Strings },
        Field { .key = "fold_empty", .type = JsonType::Bool },
    };

    constexpr auto AutomationFields = std::array {
        Field { .key = "enabled", .type = JsonType::Bool },
        Field { .key = "max_agents", .type = JsonType::Count },
        Field { .key = "rules", .type = JsonType::Objects, .object = ObjectKind::AutomationRule },
    };

    constexpr auto AutomationRuleFields = std::array {
        Field { .key = "when", .type = JsonType::String },
        Field { .key = "task", .type = JsonType::String },
        Field { .key = "worker", .type = JsonType::String },
        Field { .key = "model", .type = JsonType::String },
        Field { .key = "permissions", .type = JsonType::String },
    };

    [[nodiscard]] constexpr std::span<Field const> fieldsOf(ObjectKind kind) noexcept
    {
        switch (kind)
        {
            case ObjectKind::Config: return ConfigFields;
            case ObjectKind::Repo: return RepoFields;
            case ObjectKind::IssueSource: return IssueSourceFields;
            case ObjectKind::Task: return TaskFields;
            case ObjectKind::Plugin: return PluginFields;
            case ObjectKind::PermissionProfile: return PermissionProfileFields;
            case ObjectKind::QuickReply: return QuickReplyFields;
            case ObjectKind::Worker: return WorkerFields;
            case ObjectKind::Keys: return KeysFields;
            case ObjectKind::Work: return WorkFields;
            case ObjectKind::Automation: return AutomationFields;
            case ObjectKind::AutomationRule: return AutomationRuleFields;
        }
        return {};
    }

    /// @return Whether @p fields names exactly the keys glaze reads into @p T, in the same order: the shape
    /// walk
    ///         and the mirror read must agree, or a key one admits the other refuses without saying where.
    template <typename T>
    [[nodiscard]] constexpr bool matchesMirror(std::span<Field const> fields) noexcept
    {
        constexpr auto Keys = glz::reflect<T>::keys;
        return Keys.size() == fields.size()
               && std::ranges::equal(Keys, fields, std::ranges::equal_to {}, {}, &Field::key);
    }

    static_assert(matchesMirror<JsonConfig>(ConfigFields), "ConfigFields and JsonConfig name the same keys");
    static_assert(matchesMirror<JsonRepo>(RepoFields), "RepoFields and JsonRepo name the same keys");
    static_assert(matchesMirror<JsonIssueSource>(IssueSourceFields),
                  "IssueSourceFields and JsonIssueSource name the same keys");
    static_assert(matchesMirror<JsonTask>(TaskFields), "TaskFields and JsonTask name the same keys");
    static_assert(matchesMirror<JsonPluginRequirement>(PluginFields),
                  "PluginFields and JsonPluginRequirement name the same keys");
    static_assert(matchesMirror<JsonPermissionProfile>(PermissionProfileFields),
                  "PermissionProfileFields and JsonPermissionProfile name the same keys");
    static_assert(matchesMirror<JsonQuickReply>(QuickReplyFields),
                  "QuickReplyFields and JsonQuickReply name the same keys");
    static_assert(matchesMirror<JsonWorker>(WorkerFields), "WorkerFields and JsonWorker name the same keys");
    static_assert(matchesMirror<JsonKeys>(KeysFields), "KeysFields and JsonKeys name the same keys");
    static_assert(matchesMirror<JsonWork>(WorkFields), "WorkFields and JsonWork name the same keys");
    static_assert(matchesMirror<JsonAutomation>(AutomationFields),
                  "AutomationFields and JsonAutomation name the same keys");
    static_assert(matchesMirror<JsonAutomationRule>(AutomationRuleFields),
                  "AutomationRuleFields and JsonAutomationRule name the same keys");

    /// Integers are read exactly, so that a count is never a rounded double.
    using Generic = glz::generic_u64;

    [[nodiscard]] ConfigError problemAt(std::string key, ConfigProblem problem)
    {
        return ConfigError { .key = std::move(key), .problem = problem };
    }

    [[nodiscard]] std::optional<ConfigError> checkObject(Generic const& value,
                                                         ObjectKind kind,
                                                         std::string const& key);

    /// The largest integer a `keys` list reads as a key: a digit.
    constexpr uint64_t MaxKeyDigit = 9;

    /// @return Whether @p element may be an entry of a `keys` list: a string, or an integer 0 to 9 (YAML's
    ///         `- 0`), as a quick reply's `key` may be a bare digit.
    [[nodiscard]] bool isKeyElement(Generic const& element) noexcept
    {
        auto const* const number = std::get_if<uint64_t>(&element.data);
        return element.is_string() || (number != nullptr && *number <= MaxKeyDigit);
    }

    /// @return The first wrong type in @p value, or unknown key beneath it, for a value of @p field at @p
    /// key.
    [[nodiscard]] std::optional<ConfigError> checkValue(Generic const& value,
                                                        Field const& field,
                                                        std::string const& key)
    {
        if (std::holds_alternative<std::nullptr_t>(value.data))
            return std::nullopt;
        auto const* const number = std::get_if<uint64_t>(&value.data);
        auto const* const array = std::get_if<Generic::array_t>(&value.data);
        auto wrongType = std::optional { problemAt(key, ConfigProblem::WrongType) };
        switch (field.type)
        {
            case JsonType::String: return value.is_string() ? std::nullopt : wrongType;
            case JsonType::Count:
                return number != nullptr && *number <= std::numeric_limits<uint32_t>::max() ? std::nullopt
                                                                                            : wrongType;
            case JsonType::Duration:
            case JsonType::Key: return value.is_string() || number != nullptr ? std::nullopt : wrongType;
            case JsonType::Bool: return std::holds_alternative<bool>(value.data) ? std::nullopt : wrongType;
            case JsonType::Object: return checkObject(value, field.object, key);
            case JsonType::Strings:
            case JsonType::Objects:
            case JsonType::KeyList:
                if (array == nullptr)
                    return wrongType;
                for (auto const index: std::views::iota(std::size_t { 0 }, array->size()))
                {
                    auto const& element = (*array)[index];
                    auto const elementKey = std::format("{}[{}]", key, index);
                    if (field.type == JsonType::Strings && !element.is_string())
                        return problemAt(elementKey, ConfigProblem::WrongType);
                    if (field.type == JsonType::KeyList && !isKeyElement(element))
                        return problemAt(elementKey, ConfigProblem::WrongType);
                    if (field.type == JsonType::Objects)
                        if (auto problem = checkObject(element, field.object, elementKey))
                            return problem;
                }
                return std::nullopt;
        }
        return wrongType;
    }

    /// @return The first unknown key or wrong type in the object @p value of @p kind at @p key.
    std::optional<ConfigError> checkObject(Generic const& value, ObjectKind kind, std::string const& key)
    {
        auto const* const object = std::get_if<Generic::object_t>(&value.data);
        if (object == nullptr)
            return problemAt(key, ConfigProblem::WrongType);
        auto const fields = fieldsOf(kind);
        for (auto const& [name, child]: *object)
        {
            auto childKey = key.empty() ? name : std::format("{}.{}", key, name);
            auto const field = std::ranges::find(fields, std::string_view { name }, &Field::key);
            if (field == fields.end())
                return problemAt(std::move(childKey), ConfigProblem::UnknownKey);
            if (auto problem = checkValue(child, *field, childKey))
                return problem;
        }
        return std::nullopt;
    }

    /// @return What is wrong with @p value as a Name, if anything.
    [[nodiscard]] constexpr std::optional<ConfigProblem> nameProblem(std::string_view value) noexcept
    {
        if (argumentAccepts(ArgumentKind::Name, value))
            return std::nullopt;
        return ConfigProblem::NotAName;
    }

    /// @return What is wrong with @p value as a repository's name, which names a directory beneath the
    ///         worktree root: a Name, and on a Windows host one Win32 addresses by that spelling (not `con`,
    ///         not `x.`).
    [[nodiscard]] constexpr std::optional<ConfigProblem> repoNameProblem(std::string_view value,
                                                                         PathStyle style) noexcept
    {
        if (auto const problem = nameProblem(value))
            return problem;
        if (style == PathStyle::Windows && !detail::isPlainWindowsName(value))
            return ConfigProblem::NotAName;
        return std::nullopt;
    }

    /// @return What is wrong with @p value as a model: a plain alias or id, a Name without a bracketed
    ///         variant.
    [[nodiscard]] constexpr std::optional<ConfigProblem> modelProblem(std::string_view value) noexcept
    {
        if (value.contains('['))
            return ConfigProblem::BracketedModel;
        return nameProblem(value);
    }

    /// @return What is wrong with @p value as a non-empty Text, if anything.
    [[nodiscard]] constexpr std::optional<ConfigProblem> textProblem(std::string_view value) noexcept
    {
        if (value.empty())
            return ConfigProblem::Empty;
        if (value.starts_with('-'))
            return ConfigProblem::LeadingDash;
        if (!argumentAccepts(ArgumentKind::Text, value))
            return ConfigProblem::NotText;
        return std::nullopt;
    }

    /// @return What `status_board` asks every agent's system prompt for: the default without one, nothing for
    ///         `off`, else its own words, which must be Text.
    [[nodiscard]] std::expected<std::string, ConfigError> boardInstructionOf(
        std::optional<std::string> const& value)
    {
        if (!value)
            return std::string { DefaultBoardInstruction };
        if (*value == BoardOff)
            return std::string {};
        if (auto const problem = textProblem(*value))
            return fail("status_board", *problem);
        return *value;
    }

    /// @return What is wrong with @p value as a task key, if anything.
    [[nodiscard]] std::optional<ConfigProblem> keyProblem(std::string_view value) noexcept
    {
        if (value.size() == 1 && isDashboardKey(value.front()))
            return ConfigProblem::ReservedKey;
        if (value.size() != 1 || value.front() <= ' ' || value.front() > '~')
            return ConfigProblem::NotOneKey;
        return std::nullopt;
    }

    /// @return @p value, or the problem @p judge finds with it, reported at @p key.
    template <typename Judge>
    [[nodiscard]] Checked<std::string> checked(std::optional<std::string> const& value,
                                               std::string key,
                                               Judge judge)
    {
        if (!value)
            return fail(std::move(key), ConfigProblem::Missing);
        if (auto const problem = judge(*value))
            return fail(std::move(key), *problem);
        return *value;
    }

    [[nodiscard]] Checked<uint32_t> limitOf(std::optional<uint32_t> value,
                                            std::string_view key,
                                            uint32_t fallback)
    {
        auto const limit = value.value_or(fallback);
        if (limit == 0)
            return fail(std::string { key }, ConfigProblem::Zero);
        return limit;
    }

    [[nodiscard]] Checked<std::chrono::milliseconds> durationOf(std::optional<JsonDuration> const& value,
                                                                std::string_view key,
                                                                std::chrono::milliseconds fallback,
                                                                std::chrono::milliseconds minimum)
    {
        if (!value)
            return fallback;
        // A number carries no unit: only 0 means the same in every unit.
        auto const duration = std::visit(
            [](auto const& alternative) -> std::optional<std::chrono::milliseconds> {
                if constexpr (std::is_same_v<std::decay_t<decltype(alternative)>, uint64_t>)
                    return alternative == 0 ? std::optional { std::chrono::milliseconds { 0 } }
                                            : std::nullopt;
                else
                    return parseDuration(alternative);
            },
            *value);
        if (!duration)
            return fail(std::string { key }, ConfigProblem::NotADuration);
        if (*duration < minimum)
            return fail(std::string { key }, ConfigProblem::TooShort);
        return *duration;
    }

    [[nodiscard]] Checked<std::vector<std::string>> modelsOf(
        std::optional<std::vector<std::string>> const& value)
    {
        if (!value)
            return std::vector<std::string>(DefaultModels.begin(), DefaultModels.end());
        if (value->empty())
            return fail("models", ConfigProblem::Empty);
        auto models = std::vector<std::string> {};
        for (auto const& model: *value)
        {
            auto key = std::format("models[{}]", models.size());
            if (auto const problem = modelProblem(model))
                return fail(std::move(key), *problem);
            if (std::ranges::contains(models, model))
                return fail(std::move(key), ConfigProblem::Duplicate);
            models.push_back(model);
        }
        return models;
    }

    /// The keys of an issue source's entry that hold its query or its site, each with the value read there.
    struct SourceKey
    {
        std::string_view key;
        std::optional<std::string> const* value;
    };

    [[nodiscard]] Checked<IssueQuery> issueQueryOf(JsonIssueSource const& json,
                                                   std::string_view prefix,
                                                   SourceTables const& sources)
    {
        auto const key = [&](std::string_view field) {
            return std::format("{}.{}", prefix, field);
        };
        if (!json.source)
            return fail(key("source"), ConfigProblem::Missing);
        auto const* const row = findIssueSource(sources.issueSources, *json.source);
        if (row == nullptr)
            return fail(key("source"), ConfigProblem::UnsupportedSource);
        if (row->verification != Verification::Recorded)
            return std::unexpected(ConfigError { .key = key("source"),
                                                 .problem = ConfigProblem::Unrecorded,
                                                 .subject = std::string(row->name) });

        // A key another source reads is a mistake here, not an ignored extra.
        auto const keys = std::array {
            SourceKey { .key = "query", .value = &json.query },
            SourceKey { .key = "jql", .value = &json.jql },
            SourceKey { .key = "in_progress_jql", .value = &json.inProgressJql },
            SourceKey { .key = "site", .value = &json.site },
        };
        for (auto const& [name, value]: keys)
        {
            auto const used = name == row->configKey || name == row->progressKey
                              || (name == "site" && row->site == SiteUse::Required);
            if (value->has_value() && !used)
                return fail(key(name), ConfigProblem::UnknownKey);
        }

        auto const query = std::ranges::find(keys, row->configKey, &SourceKey::key);
        if (query == keys.end()) // a row naming a key no entry has: a defect, never a valid entry
            return fail(key("source"), ConfigProblem::UnsupportedSource);
        auto text = checked(*query->value, key(row->configKey), textProblem);
        if (!text)
            return std::unexpected(text.error());
        auto progress = std::string {};
        if (json.inProgressJql)
        {
            auto const checkedProgress = checked(json.inProgressJql, key(row->progressKey), textProblem);
            if (!checkedProgress)
                return std::unexpected(checkedProgress.error());
            // The two are sent as one query, `(jql) AND (in_progress_jql)`, which must fit a Text too.
            if (std::format("({}) AND ({})", *text, *checkedProgress).size() > MaxTextBytes)
                return fail(key(row->progressKey), ConfigProblem::NotText);
            progress = *checkedProgress;
        }
        auto site = std::string {};
        if (row->site == SiteUse::Required)
        {
            if (!json.site)
                return fail(key("site"), ConfigProblem::Missing);
            auto const checkedSite = jira::siteOf(*json.site);
            if (!checkedSite)
                return fail(key("site"), ConfigProblem::NotASite);
            site = std::string(*checkedSite);
        }
        return IssueQuery { .source = std::string(row->name),
                            .text = std::move(*text),
                            .site = std::move(site),
                            .inProgressJql = std::move(progress) };
    }

    /// @return The name a repository is known by: the one written at `name`, or else its directory's.
    [[nodiscard]] Checked<std::string> repoNameOf(std::optional<std::string> const& written,
                                                  std::string_view path,
                                                  PathStyle style,
                                                  std::string_view prefix,
                                                  std::span<Repo const> earlier)
    {
        if (written)
        {
            auto key = std::format("{}.name", prefix);
            if (auto const problem = repoNameProblem(*written, style))
                return fail(std::move(key), *problem);
            if (std::ranges::contains(earlier, *written, &Repo::name))
                return fail(std::move(key), ConfigProblem::Duplicate);
            return *written;
        }
        // Unwritten, the name is the directory's, and a problem with it is the path's.
        auto key = std::format("{}.path", prefix);
        auto const name = lastComponent(path, style);
        if (nameProblem(name))
            return fail(std::move(key), ConfigProblem::UnnamedDirectory);
        if (std::ranges::contains(earlier, name, &Repo::name))
            return fail(std::move(key), ConfigProblem::DuplicateDirectoryName);
        return std::string { name };
    }

    /// @return The forge @p name spells, if @p sources has a Recorded row for it; else why not.
    [[nodiscard]] Checked<Forge> forgeOf(std::string_view name, std::string key, SourceTables const& sources)
    {
        auto const forge = forgeNamed(name);
        auto const provider =
            forge ? std::ranges::find(sources.providers, *forge, &Provider::forge) : sources.providers.end();
        if (provider == sources.providers.end())
            return fail(std::move(key), ConfigProblem::UnknownForge);
        if (provider->verification != Verification::Recorded)
            return std::unexpected(ConfigError { .key = std::move(key),
                                                 .problem = ConfigProblem::Unrecorded,
                                                 .subject = std::string(provider->name) });
        return *forge;
    }

    /// Validates the repository at @p index against those before it.
    [[nodiscard]] Checked<Repo> repoOf(JsonRepo const& json,
                                       std::size_t index,
                                       std::string_view path,
                                       std::string_view worktreeRoot,
                                       PathStyle style,
                                       std::span<Repo const> earlier,
                                       SourceTables const& sources)
    {
        auto const prefix = std::format("repos[{}]", index);
        auto const key = [&](std::string_view field) {
            return std::format("{}.{}", prefix, field);
        };

        if (!json.path)
            return fail(key("path"), ConfigProblem::Missing);
        // A repository is spelled in the worktree root's style: one host spells all its paths alike.
        if (!argumentAccepts(ArgumentKind::Path, path) || pathStyleOf(path) != style)
            return fail(key("path"), ConfigProblem::NotAbsolutePath);
        auto const isSame = [&](std::string_view other) {
            return sameHostPath(path, other, style);
        };
        if (isSame(worktreeRoot) || std::ranges::any_of(earlier, isSame, &Repo::path))
            return fail(key("path"), ConfigProblem::EqualRoots);
        // A worktree root inside a repository is fine: a directory belongs to its most specific root.
        if (isStrictlyInside(path, worktreeRoot, style))
            return fail(key("path"), ConfigProblem::InsideWorktreeRoot);

        if (!json.forge)
            return fail(key("forge"), ConfigProblem::Missing);
        auto const forge = forgeOf(*json.forge, key("forge"), sources);
        if (!forge)
            return std::unexpected(forge.error());

        auto const name = repoNameOf(json.name, path, style, prefix, earlier);
        if (!name)
            return std::unexpected(name.error());

        auto const remote = checked(std::optional { json.remote.value_or(std::string { DefaultRemote }) },
                                    key("remote"),
                                    nameProblem);
        if (!remote)
            return std::unexpected(remote.error());

        if (json.base && !argumentAccepts(ArgumentKind::Ref, *json.base))
            return fail(key("base"), ConfigProblem::NotARef);

        auto issueQueries = std::vector<IssueQuery> {};
        for (auto const& source:
             json.issues ? std::span { *json.issues } : std::span<JsonIssueSource const> {})
        {
            auto query =
                issueQueryOf(source, std::format("{}.issues[{}]", prefix, issueQueries.size()), sources);
            if (!query)
                return std::unexpected(query.error());
            issueQueries.push_back(std::move(*query));
        }

        return Repo { .name = *name,
                      .path = std::string { path },
                      .forge = *forge,
                      .remote = *remote,
                      .base = json.base,
                      .issueQueries = std::move(issueQueries) };
    }

    [[nodiscard]] Checked<std::vector<Repo>> reposOf(std::optional<std::vector<JsonRepo>> const& value,
                                                     HostInfo const& host,
                                                     PathStyle style,
                                                     SourceTables const& sources)
    {
        if (!value)
            return fail("repos", ConfigProblem::Missing);
        if (value->empty())
            return fail("repos", ConfigProblem::Empty);
        if (value->size() != host.repoPaths.size())
            return fail("repos", ConfigProblem::CountMismatch);
        auto repos = std::vector<Repo> {};
        for (auto const index: std::views::iota(std::size_t { 0 }, value->size()))
        {
            auto repo = repoOf(
                (*value)[index], index, host.repoPaths[index], host.worktreeRoot, style, repos, sources);
            if (!repo)
                return std::unexpected(repo.error());
            repos.push_back(std::move(*repo));
        }
        return repos;
    }

    /// @return The workers a configuration names, each checked; without `workers`, the implicit one.
    [[nodiscard]] Checked<std::vector<WorkerDef>> workersOf(
        std::optional<std::vector<JsonWorker>> const& value)
    {
        if (!value)
            return std::vector<WorkerDef> { WorkerDef {
                .id = std::string { ImplicitWorkerId }, .agent = AgentKind::Claude, .envSet = {} } };
        if (value->empty())
            return fail("workers", ConfigProblem::Empty);
        auto workers = std::vector<WorkerDef> {};
        for (auto const [index, json]: std::views::enumerate(*value))
        {
            auto const key = [&](std::string_view field) {
                return std::format("workers[{}].{}", index, field);
            };
            auto const id = checked(json.id, key("id"), nameProblem);
            if (!id)
                return std::unexpected(id.error());
            if (std::ranges::contains(workers, *id, &WorkerDef::id))
                return fail(key("id"), ConfigProblem::Duplicate);
            if (!json.agent)
                return fail(key("agent"), ConfigProblem::Missing);
            auto const* const agent = findAgent(*json.agent);
            if (agent == nullptr)
                return fail(key("agent"), ConfigProblem::UnknownAgent);
            auto envSet = std::string {};
            if (json.envSet)
            {
                auto const set = checked(json.envSet, key("env_set"), nameProblem);
                if (!set)
                    return std::unexpected(set.error());
                envSet = *set;
            }
            workers.push_back(WorkerDef { .id = *id, .agent = agent->kind, .envSet = std::move(envSet) });
        }
        return workers;
    }

    /// Validates the task at @p index against those before it. A task naming no worker runs on the first
    /// one, and a notice says so.
    [[nodiscard]] Checked<TaskDef> taskOf(JsonTask const& json,
                                          std::size_t index,
                                          std::span<std::string const> models,
                                          std::span<TaskDef const> earlier,
                                          std::span<WorkerDef const> workers,
                                          std::vector<ConfigError>& notices)
    {
        auto const key = [index](std::string_view field) {
            return std::format("tasks[{}].{}", index, field);
        };

        auto const id = checked(json.id, key("id"), nameProblem);
        if (!id)
            return std::unexpected(id.error());
        if (std::ranges::contains(earlier, *id, &TaskDef::id))
            return fail(key("id"), ConfigProblem::Duplicate);

        auto const keyText = checked(json.key, key("key"), keyProblem);
        if (!keyText)
            return std::unexpected(keyText.error());
        if (std::ranges::contains(earlier, keyText->front(), &TaskDef::key))
            return fail(key("key"), ConfigProblem::Duplicate);

        auto const label = checked(json.label, key("label"), [](std::string_view value) {
            auto const problem = textProblem(value);
            // A label is shown, never passed as an argument: a leading '-' is harmless there.
            return problem == ConfigProblem::LeadingDash ? std::nullopt : problem;
        });
        if (!label)
            return std::unexpected(label.error());

        if (!json.appliesTo)
            return fail(key("applies_to"), ConfigProblem::Missing);
        auto const appliesTo = meaningOf(AppliesToSpellings, *json.appliesTo);
        if (!appliesTo)
            return fail(key("applies_to"), ConfigProblem::UnknownItemKind);

        auto const model = checked(json.model, key("model"), modelProblem);
        if (!model)
            return std::unexpected(model.error());
        if (!std::ranges::contains(models, *model))
            return fail(key("model"), ConfigProblem::UnlistedModel);

        auto const prompt = checked(json.prompt, key("prompt"), [&](std::string_view value) {
            auto const problem = textProblem(value);
            if (problem)
                return problem;
            // A topic has no item, so neither a URL nor a key: its text goes where `{topic}` is, or after the
            // prompt.
            if (*appliesTo == AppliesTo::Topic)
                return value.contains(KeyPlaceholder) ? std::optional { ConfigProblem::KeyInTopic } : problem;
            // An item's prompt names it by its URL, or by its key.
            if (!value.contains(UrlPlaceholder) && !value.contains(KeyPlaceholder))
                return std::optional { ConfigProblem::MissingUrl };
            return problem;
        });
        if (!prompt)
            return std::unexpected(prompt.error());

        auto endDoneAfter = std::optional<std::chrono::milliseconds> {};
        if (json.endDoneAfter)
        {
            auto const duration = durationOf(json.endDoneAfter,
                                             key("end_done_after"),
                                             DefaultEndDoneAfter,
                                             std::chrono::milliseconds { 0 });
            if (!duration)
                return std::unexpected(duration.error());
            endDoneAfter = *duration;
        }

        auto permissions = std::string {};
        if (json.permissions)
        {
            auto const profile = checked(json.permissions, key("permissions"), nameProblem);
            if (!profile)
                return std::unexpected(profile.error());
            permissions = *profile;
        }

        auto worker = std::string { workers.front().id };
        if (json.worker)
        {
            auto const named = checked(json.worker, key("worker"), nameProblem);
            if (!named)
                return std::unexpected(named.error());
            if (std::ranges::contains(workers, *named, &WorkerDef::id))
                worker = *named;
            else
                notices.push_back(ConfigError {
                    .key = key("worker"), .problem = ConfigProblem::UnknownWorker, .subject = *named });
        }

        // A role written is the one it names; without one, the role the id spells, else none.
        auto role = taskRoleNamed(*id).value_or(TaskRole::None);
        if (json.role)
        {
            auto const named = taskRoleNamed(*json.role);
            if (!named)
                return fail(key("role"), ConfigProblem::UnknownRole);
            role = *named;
        }

        return TaskDef { .id = *id,
                         .key = keyText->front(),
                         .label = *label,
                         .appliesTo = *appliesTo,
                         .model = *model,
                         .prompt = *prompt,
                         .endDoneAfter = endDoneAfter,
                         .permissions = std::move(permissions),
                         .worker = std::move(worker),
                         .role = role };
    }

    [[nodiscard]] Checked<std::vector<TaskDef>> tasksOf(std::optional<std::vector<JsonTask>> const& value,
                                                        std::span<std::string const> models,
                                                        std::span<WorkerDef const> workers,
                                                        std::vector<ConfigError>& notices)
    {
        if (!value)
            return fail("tasks", ConfigProblem::Missing);
        if (value->empty())
            return fail("tasks", ConfigProblem::Empty);
        auto tasks = std::vector<TaskDef> {};
        for (auto const& json: *value)
        {
            auto task = taskOf(json, tasks.size(), models, tasks, workers, notices);
            if (!task)
                return std::unexpected(task.error());
            tasks.push_back(std::move(*task));
        }
        return tasks;
    }

    /// @return Whether @p id is `<plugin>@<marketplace>`: one `@` with something on either side, and no
    /// space.
    [[nodiscard]] constexpr bool isPluginId(std::string_view id) noexcept
    {
        auto const at = id.find('@');
        return at != std::string_view::npos && at > 0 && at + 1 < id.size()
               && id.find('@', at + 1) == std::string_view::npos && !id.contains(' ');
    }

    [[nodiscard]] Checked<std::vector<PluginRequirement>> pluginsOf(
        std::optional<std::vector<JsonPluginRequirement>> const& value)
    {
        if (!value)
            return defaultPluginRequirements();
        auto plugins = std::vector<PluginRequirement> {};
        for (auto const [index, json]: std::views::enumerate(*value))
        {
            auto const key = [&](std::string_view field) {
                return std::format("plugins[{}].{}", index, field);
            };
            if (!json.id)
                return fail(key("id"), ConfigProblem::Missing);
            if (auto const problem = textProblem(*json.id))
                return fail(key("id"), *problem);
            if (!isPluginId(*json.id))
                return fail(key("id"), ConfigProblem::NotPluginId);
            if (std::ranges::contains(plugins, *json.id, &PluginRequirement::id))
                return fail(key("id"), ConfigProblem::Duplicate);
            auto marketplace = json.marketplace.value_or(std::string {});
            if (!marketplace.empty())
                if (auto const problem = textProblem(marketplace))
                    return fail(key("marketplace"), *problem);
            plugins.push_back(PluginRequirement { .id = *json.id, .marketplace = std::move(marketplace) });
        }
        return plugins;
    }

    /// @return @p key as text: a bare number as its digits.
    [[nodiscard]] std::string keyText(JsonKey const& key)
    {
        return std::visit(Overloaded {
                              [](uint64_t number) { return std::to_string(number); },
                              [](std::string const& text) { return text; },
                          },
                          key);
    }

    [[nodiscard]] Checked<std::vector<QuickReply>> quickRepliesOf(
        std::optional<std::vector<JsonQuickReply>> const& value)
    {
        if (!value)
            return defaultQuickReplies();
        auto replies = std::vector<QuickReply> {};
        for (auto const [index, json]: std::views::enumerate(*value))
        {
            auto const key = [&](std::string_view field) {
                return std::format("quick_replies[{}].{}", index, field);
            };
            if (!json.key)
                return fail(key("key"), ConfigProblem::Missing);
            auto const digit = keyText(*json.key);
            if (digit.size() != 1 || digit.front() < '1' || digit.front() > '9')
                return fail(key("key"), ConfigProblem::NotAReplyKey);
            if (findQuickReply(replies, digit.front()) != nullptr)
                return fail(key("key"), ConfigProblem::Duplicate);
            if (!json.label)
                return fail(key("label"), ConfigProblem::Missing);
            if (json.label->empty())
                return fail(key("label"), ConfigProblem::Empty);
            if (auto const problem = textProblem(*json.label))
                return fail(key("label"), *problem);
            if (json.label->size() > MaxQuickReplyLabelBytes)
                return fail(key("label"), ConfigProblem::LabelTooLong);
            auto const press = paneKeyNamed(json.press ? keyText(*json.press) : digit);
            if (!press)
                return fail(key("press"), ConfigProblem::NotAPaneKey);
            auto const then = json.then ? afterReplyNamed(*json.then) : std::optional { AfterReply::Wait };
            if (!then)
                return fail(key("then"), ConfigProblem::UnknownThen);
            replies.push_back(
                QuickReply { .key = digit.front(), .label = *json.label, .press = *press, .then = *then });
        }
        return replies;
    }

    /// The problem a refused `keys` entry is reported as.
    struct KeyRefusalProblem
    {
        KeyRefusal refusal;
        ConfigProblem problem;
    };

    constexpr auto KeyRefusalProblems = std::array {
        KeyRefusalProblem { .refusal = KeyRefusal::NotAKey, .problem = ConfigProblem::NotAKeyName },
        KeyRefusalProblem { .refusal = KeyRefusal::TypesText, .problem = ConfigProblem::TypesText },
        KeyRefusalProblem { .refusal = KeyRefusal::Taken, .problem = ConfigProblem::KeyTaken },
        KeyRefusalProblem { .refusal = KeyRefusal::Unbound, .problem = ConfigProblem::Empty },
    };
    static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, KeyRefusalProblems.size()),
                                      [](std::size_t i) {
                                          return static_cast<std::size_t>(KeyRefusalProblems[i].refusal) == i;
                                      }),
                  "KeyRefusalProblems has one row per KeyRefusal, in its order");

    /// @return @p problem as the configuration reports it: at `keys.<action>[<i>]`, or at `keys.<action>` for
    ///         an action left without keys.
    [[nodiscard]] ConfigError keyErrorOf(KeyBindingProblem problem)
    {
        auto const name = navActionRows()[static_cast<std::size_t>(problem.action)].configName;
        return ConfigError {
            .key = problem.index ? std::format("keys.{}[{}]", name, *problem.index)
                                 : std::format("keys.{}", name),
            .problem = KeyRefusalProblems[static_cast<std::size_t>(problem.refusal)].problem,
            .subject = std::move(problem.subject),
        };
    }

    /// @return The navigation keys `keys` asks for; without it, the defaults with the vim keys.
    [[nodiscard]] Checked<NavKeys> keysOf(std::optional<JsonKeys> const& value)
    {
        if (!value)
            return NavKeys::defaults(VimKeys::On);
        auto const vim = value->vim.value_or(true) ? VimKeys::On : VimKeys::Off;
        auto configured = std::array<std::optional<std::vector<std::string>>, NavActionCount> {};
        for (auto const [index, member]: std::views::enumerate(KeyListMembers))
            if (auto const& list = (*value).*member)
                configured[static_cast<std::size_t>(index)] =
                    *list | std::views::transform(keyText) | std::ranges::to<std::vector>();
        return navKeysOf(vim, configured).transform_error(keyErrorOf);
    }

    /// @return The Work board's settings `work` asks for: the groups it names folded at start, each once and
    ///         in row order, and whether empty groups show; without it, the defaults.
    [[nodiscard]] Checked<WorkSettings> workOf(std::optional<JsonWork> const& value)
    {
        if (!value)
            return defaultWorkSettings();
        auto settings = defaultWorkSettings();
        if (value->foldEmpty)
            settings.empty = *value->foldEmpty ? EmptyGroups::Hidden : EmptyGroups::Shown;
        if (!value->folded)
            return settings;
        auto const& names = *value->folded;
        for (auto const [index, name]: std::views::enumerate(names))
            if (std::ranges::none_of(WorkGroupRows,
                                     [&](WorkGroupRow const& row) { return row.configName == name; }))
                return fail(std::format("work.folded[{}]", index), ConfigProblem::UnknownGroup);
        settings.folded = WorkGroupRows | std::views::filter([&](WorkGroupRow const& row) {
                              return std::ranges::any_of(
                                  names, [&](std::string const& name) { return name == row.configName; });
                          })
                          | std::views::transform(&WorkGroupRow::group) | std::ranges::to<std::vector>();
        return settings;
    }

    /// What a rule is checked against: what the configuration read before `automation`.
    struct RuleTables
    {
        std::span<std::string const> models;         ///< `models`.
        std::span<TaskDef const> tasks;              ///< The tasks, the unavailable ones left out.
        std::span<WorkerDef const> workers;          ///< The workers.
        std::span<PermissionProfile const> profiles; ///< The resolved permission profiles.
    };

    /// @return The rule @p json spells, checked against @p tables, reported at `automation.rules[<index>]`.
    [[nodiscard]] Checked<AutomationRule> ruleOf(JsonAutomationRule const& json,
                                                 std::size_t index,
                                                 RuleTables const& tables)
    {
        auto const key = [index](std::string_view field) {
            return std::format("automation.rules[{}].{}", index, field);
        };
        if (!json.when)
            return fail(key("when"), ConfigProblem::Missing);
        auto const when = automationConditionNamed(*json.when);
        if (!when)
            return fail(key("when"), ConfigProblem::UnknownCondition);
        if (!json.task)
            return fail(key("task"), ConfigProblem::Missing);
        auto const task = std::ranges::find(tables.tasks, *json.task, &TaskDef::id);
        if (task == tables.tasks.end())
            return fail(key("task"), ConfigProblem::UnknownRuleTask);
        if (task->appliesTo != automationConditionRow(*when).appliesTo)
            return fail(key("task"), ConfigProblem::RuleTaskMismatch);
        auto rule = AutomationRule { .when = *when,
                                     .task = *json.task,
                                     .worker = json.worker.value_or(std::string {}),
                                     .model = json.model.value_or(std::string {}),
                                     .permissions = json.permissions.value_or(std::string {}) };
        if (!rule.worker.empty())
        {
            auto const worker = std::ranges::find(tables.workers, rule.worker, &WorkerDef::id);
            if (worker == tables.workers.end())
                return fail(key("worker"), ConfigProblem::UnknownRuleWorker);
            // The rule's own profile stands for the task's on this worker.
            auto withProfile = *task;
            if (!rule.permissions.empty())
                withProfile.permissions = rule.permissions;
            if (takesNoProfileFor(withProfile, *worker))
                return fail(key("worker"), ConfigProblem::RuleWorkerTakesNoProfile);
        }
        if (!rule.model.empty() && !std::ranges::contains(tables.models, rule.model))
            return fail(key("model"), ConfigProblem::UnlistedModel);
        if (!rule.permissions.empty() && findProfile(tables.profiles, rule.permissions) == nullptr)
            return fail(key("permissions"), ConfigProblem::UnknownProfile);
        return rule;
    }

    /// @return The settings `automation` asks for; without it, none: no rule, and the switch paused.
    [[nodiscard]] Checked<AutomationSettings> automationOf(std::optional<JsonAutomation> const& value,
                                                           RuleTables const& tables)
    {
        if (!value)
            return AutomationSettings {};
        auto settings = AutomationSettings {
            .enabled = value->enabled.value_or(false) ? AutomationSwitch::On : AutomationSwitch::Paused,
        };
        if (value->maxAgents)
        {
            auto const cap = limitOf(value->maxAgents, "automation.max_agents", 1);
            if (!cap)
                return std::unexpected(cap.error());
            settings.maxAgents = *cap;
        }
        if (!value->rules)
            return settings;
        for (auto const [index, json]: std::views::enumerate(*value->rules))
        {
            auto rule = ruleOf(json, static_cast<std::size_t>(index), tables);
            if (!rule)
                return std::unexpected(std::move(rule.error()));
            settings.rules.push_back(std::move(*rule));
        }
        return settings;
    }

    /// @return The rules of a profile's @p list, each checked, reported at `<key>[i]`.
    [[nodiscard]] Checked<std::vector<std::string>> checkedRules(
        std::optional<std::vector<std::string>> const& list, std::string const& key)
    {
        if (!list)
            return std::vector<std::string> {};
        for (auto const [index, rule]: std::views::enumerate(*list))
        {
            auto const at = std::format("{}[{}]", key, index);
            if (auto const problem = textProblem(rule))
                return fail(at, *problem);
            if (!isPermissionRule(rule))
                return fail(at, ConfigProblem::NotPermissionRule);
        }
        return *list;
    }

    /// Validates the profile at @p index against those before it.
    [[nodiscard]] Checked<ProfileDef> profileOf(JsonPermissionProfile const& json,
                                                std::size_t index,
                                                std::span<ProfileDef const> earlier)
    {
        auto const key = [index](std::string_view field) {
            return std::format("permission_profiles[{}].{}", index, field);
        };
        auto const name = checked(json.name, key("name"), nameProblem);
        if (!name)
            return std::unexpected(name.error());
        if (std::ranges::contains(earlier, *name, &ProfileDef::name))
            return fail(key("name"), ConfigProblem::Duplicate);
        auto profile = ProfileDef { .name = *name };
        if (json.extends)
        {
            auto const parent = checked(json.extends, key("extends"), nameProblem);
            if (!parent)
                return std::unexpected(parent.error());
            profile.extends = *parent;
        }
        if (json.mode)
        {
            if (*json.mode == RefusedPermissionMode)
                return fail(key("mode"), ConfigProblem::BypassRefused);
            profile.mode = permissionModeOf(*json.mode);
            if (!profile.mode)
                return fail(key("mode"), ConfigProblem::NotPermissionMode);
        }
        auto allow = checkedRules(json.allow, key("allow"));
        if (!allow)
            return std::unexpected(allow.error());
        auto deny = checkedRules(json.deny, key("deny"));
        if (!deny)
            return std::unexpected(deny.error());
        profile.allow = std::move(*allow);
        profile.deny = std::move(*deny);
        return profile;
    }

    /// @return Whether @p task has its own permission profile on a worker whose agent takes none: such a task
    ///         is unavailable.
    [[nodiscard]] bool profileRefused(TaskDef const& task, std::span<WorkerDef const> workers) noexcept
    {
        auto const worker = std::ranges::find(workers, task.worker, &WorkerDef::id);
        return worker != workers.end() && takesNoProfileFor(task, *worker);
    }

    /// The permission profiles, resolved, and the default's name.
    struct Permissions
    {
        std::vector<PermissionProfile> profiles;
        std::string defaultProfile;
    };

    /// Reads `permission_profiles` and `permissions`, and checks the profile each of @p tasks names.
    [[nodiscard]] Checked<Permissions> permissionsOf(
        std::optional<std::string> const& defaultName,
        std::optional<std::vector<JsonPermissionProfile>> const& configured,
        std::span<TaskDef const> tasks)
    {
        auto profiles = std::vector<ProfileDef> {};
        for (auto const [index, json]: std::views::enumerate(
                 configured ? std::span { *configured } : std::span<JsonPermissionProfile const> {}))
        {
            auto profile = profileOf(json, static_cast<std::size_t>(index), profiles);
            if (!profile)
                return std::unexpected(profile.error());
            profiles.push_back(std::move(*profile));
        }
        // Where a problem lies: the configured profile's `extends`, or the list as a whole for a built-in
        // one.
        auto const keyOf = [&](std::string_view name, std::string_view field) {
            auto const at = std::ranges::find(profiles, name, &ProfileDef::name);
            return at == profiles.end()
                       ? std::string { "permission_profiles" }
                       : std::format("permission_profiles[{}]{}", std::distance(profiles.begin(), at), field);
        };
        auto resolved = resolveProfiles(profiles);
        if (!resolved)
        {
            auto const problem = resolved.error().error == ProfileError::UnknownProfile
                                     ? ConfigProblem::UnknownProfile
                                     : ConfigProblem::ProfileCycle;
            return fail(keyOf(resolved.error().profile, ".extends"), problem);
        }
        for (auto const& profile: *resolved)
            if (!argumentAccepts(ArgumentKind::Text, settingsJsonOf(profile)))
                return fail(keyOf(profile.name, ""), ConfigProblem::SettingsTooLarge);

        auto const exists = [&](std::string_view name) {
            return findProfile(*resolved, name) != nullptr;
        };
        auto defaultProfile = std::string { DefaultProfileName };
        if (defaultName)
        {
            auto const name = checked(defaultName, "permissions", nameProblem);
            if (!name)
                return std::unexpected(name.error());
            if (!exists(*name))
                return fail("permissions", ConfigProblem::UnknownProfile);
            defaultProfile = *name;
        }
        for (auto const [index, task]: std::views::enumerate(tasks))
            if (!task.permissions.empty() && !exists(task.permissions))
                return fail(std::format("tasks[{}].permissions", index), ConfigProblem::UnknownProfile);
        return Permissions { .profiles = std::move(*resolved), .defaultProfile = std::move(defaultProfile) };
    }

} // namespace

std::string describe(ConfigProblem problem, SourceTables const& sources)
{
    auto const index = static_cast<std::size_t>(problem);
    if (index >= ProblemTexts.size())
        return {};
    auto const& row = ProblemTexts[index];
    auto text = std::string { row.text };
    auto separator = ": "sv;
    for (auto const value: listed(row.listing, sources))
    {
        text += separator;
        text += value;
        separator = ", "sv;
    }
    return text;
}

std::string message(ConfigError const& error, SourceTables const& sources)
{
    auto text = error.key.empty() ? describe(error.problem, sources)
                                  : std::format("{}: {}", error.key, describe(error.problem, sources));
    if (auto const index = static_cast<std::size_t>(error.problem);
        !error.subject.empty() && index < ProblemTexts.size())
        text += std::format(": {}{}", error.subject, ProblemTexts[index].afterSubject);
    return text;
}

SourceTables sourceTables() noexcept
{
    return SourceTables { .providers = providers(), .issueSources = issueSources() };
}

std::optional<std::chrono::milliseconds> parseDuration(std::string_view text) noexcept
{
    if (text == "0")
        return std::chrono::milliseconds { 0 };
    if (text.size() < 2)
        return std::nullopt;
    auto const digits = text.substr(0, text.size() - 1);
    auto const unit = std::ranges::find(DurationUnits, text.back(), &DurationUnit::suffix);
    if (unit == DurationUnits.end() || digits.size() > MaxDurationDigits
        || !std::ranges::all_of(digits, [](char c) { return c >= '0' && c <= '9'; }))
        return std::nullopt;
    auto count = int64_t { 0 };
    for (auto const digit: digits)
        count = (count * 10) + (digit - '0');
    return unit->length * count;
}

std::expected<Config, ConfigError> Config::parse(std::string_view json,
                                                 HostInfo const& host,
                                                 SourceTables const& sources)
{
    auto const generic = orchestrator::json::readDocument<orchestrator::json::ValidateOptions, Generic>(json);
    if (!generic)
        return fail({}, ConfigProblem::NotJson);
    if (auto problem = checkObject(*generic, ObjectKind::Config, {}))
        return std::unexpected(std::move(*problem));
    // The walk above admitted only keys and types the mirror structs read, so this read does not fail; should
    // it ever, the document as a whole is reported.
    auto document = JsonConfig {};
    if (glz::read<ReadOptions>(document, json))
        return fail({}, ConfigProblem::WrongType);

    if (!document.worktreeRoot)
        return fail("worktree_root", ConfigProblem::Missing);
    auto const style = pathStyleOf(host.worktreeRoot);
    if (!style || !argumentAccepts(ArgumentKind::Path, host.worktreeRoot))
        return fail("worktree_root", ConfigProblem::NotAbsolutePath);

    auto values = Values { .worktreeRoot = host.worktreeRoot, .pathStyle = *style, .sources = sources };
    auto const assign = [](auto& target, auto checkedValue) -> std::optional<ConfigError> {
        if (!checkedValue)
            return std::move(checkedValue.error());
        target = std::move(*checkedValue);
        return std::nullopt;
    };
    using std::chrono::milliseconds;
    constexpr auto NoMinimum = milliseconds { 0 };
    // The fields in the order they are checked: the first problem found is the one reported.
    auto const problem = [&]() -> std::optional<ConfigError> {
        if (auto e = assign(values.models, modelsOf(document.models)))
            return e;
        if (auto e = assign(values.maxAgents, limitOf(document.maxAgents, "max_agents", DefaultMaxAgents)))
            return e;
        if (auto e =
                assign(values.maxAgentsPerRepo,
                       limitOf(document.maxAgentsPerRepo, "max_agents_per_repo", DefaultMaxAgentsPerRepo)))
            return e;
        if (auto e = assign(values.pollEvery,
                            durationOf(document.pollEvery, "poll_every", DefaultPollEvery, MinPollEvery)))
            return e;
        if (auto e = assign(
                values.statusBoardAfter,
                durationOf(
                    document.statusBoardAfter, "status_board_after", DefaultStatusBoardAfter, NoMinimum)))
            return e;
        if (auto e = assign(values.boardInstruction, boardInstructionOf(document.statusBoard)))
            return e;
        if (auto e =
                assign(values.endDoneAfter,
                       durationOf(document.endDoneAfter, "end_done_after", DefaultEndDoneAfter, NoMinimum)))
            return e;
        if (auto e = assign(
                values.finishedRetention,
                durationOf(
                    document.finishedRetention, "finished_retention", DefaultFinishedRetention, NoMinimum)))
            return e;
        if (auto e = assign(values.repos, reposOf(document.repos, host, values.pathStyle, sources)))
            return e;
        if (auto e = assign(values.workers, workersOf(document.workers)))
            return e;
        if (auto e =
                assign(values.tasks, tasksOf(document.tasks, values.models, values.workers, values.notices)))
            return e;
        if (auto e = assign(values.plugins, pluginsOf(document.plugins)))
            return e;
        if (auto e = assign(values.quickReplies, quickRepliesOf(document.quickReplies)))
            return e;
        if (auto e = assign(values.keys, keysOf(document.keys)))
            return e;
        if (auto e = assign(values.work, workOf(document.work)))
            return e;
        auto permissions = permissionsOf(document.permissions, document.permissionProfiles, values.tasks);
        if (!permissions)
            return std::move(permissions.error());
        values.profiles = std::move(permissions->profiles);
        values.defaultProfile = std::move(permissions->defaultProfile);
        // A task on a worker that takes no profile cannot have one: it is unavailable, and the rest loads.
        for (auto const [index, task]: std::views::enumerate(values.tasks))
            if (profileRefused(task, values.workers))
                values.notices.push_back(ConfigError { .key = std::format("tasks[{}].permissions", index),
                                                       .problem = ConfigProblem::ProfileOnCopilot });
        std::erase_if(values.tasks,
                      [&](TaskDef const& task) { return profileRefused(task, values.workers); });
        if (auto e = assign(values.automation,
                            automationOf(document.automation,
                                         RuleTables { .models = values.models,
                                                      .tasks = values.tasks,
                                                      .workers = values.workers,
                                                      .profiles = values.profiles })))
            return e;
        return std::nullopt;
    }();
    if (problem)
        return std::unexpected(*problem);
    return Config { std::move(values) };
}

Config::Config(Values values) noexcept:
    _worktreeRoot { std::move(values.worktreeRoot) },
    _pathStyle { values.pathStyle },
    _models { std::move(values.models) },
    _maxAgents { values.maxAgents },
    _maxAgentsPerRepo { values.maxAgentsPerRepo },
    _pollEvery { values.pollEvery },
    _statusBoardAfter { values.statusBoardAfter },
    _boardInstruction { std::move(values.boardInstruction) },
    _endDoneAfter { values.endDoneAfter },
    _finishedRetention { values.finishedRetention },
    _repos { std::move(values.repos) },
    _tasks { std::move(values.tasks) },
    _workers { std::move(values.workers) },
    _notices { std::move(values.notices) },
    _plugins { std::move(values.plugins) },
    _quickReplies { std::move(values.quickReplies) },
    _profiles { std::move(values.profiles) },
    _defaultProfile { std::move(values.defaultProfile) },
    _keys { std::move(values.keys) },
    _work { std::move(values.work) },
    _automation { std::move(values.automation) },
    _sources { values.sources }
{
    // Every configuration can start an agent on a topic: without a task of its own, the topic's text is the
    // prompt.
    if (std::ranges::contains(_tasks, AppliesTo::Topic, &TaskDef::appliesTo) || _models.empty())
        return;
    auto const id = std::ranges::find_if(BuiltinTopicTaskIds, [&](std::string_view candidate) {
        return !std::ranges::contains(_tasks, candidate, &TaskDef::id);
    });
    if (id == BuiltinTopicTaskIds.end())
        return;
    _tasks.push_back(TaskDef { .id = std::string { *id },
                               .key = '\0',
                               .label = "topic",
                               .appliesTo = AppliesTo::Topic,
                               .model = _models.front(),
                               .prompt = std::string { TopicPlaceholder },
                               .endDoneAfter = std::nullopt,
                               .worker = _workers.front().id });
}

PermissionProfile const* Config::findProfile(std::string_view name) const noexcept
{
    return orchestrator::findProfile(_profiles, name);
}

PermissionProfile const& Config::defaultProfile() const noexcept
{
    // parse() resolved the profiles and checked the default names one of them.
    return *findProfile(_defaultProfile);
}

PermissionProfile const& Config::profileFor(std::string_view taskId) const noexcept
{
    auto const* const task = findTask(taskId);
    if (task == nullptr || task->permissions.empty())
        return defaultProfile();
    // parse() checked that every task's profile exists.
    return *findProfile(task->permissions);
}

Repo const* Config::findRepo(std::string_view name) const noexcept
{
    auto const repo = std::ranges::find(_repos, name, &Repo::name);
    return repo != _repos.end() ? &*repo : nullptr;
}

std::chrono::milliseconds Config::endDoneAfterFor(std::string_view taskId) const noexcept
{
    auto const* const task = findTask(taskId);
    return task != nullptr ? task->endDoneAfter.value_or(_endDoneAfter) : _endDoneAfter;
}

TaskDef const* Config::findTask(std::string_view id) const noexcept
{
    auto const task = std::ranges::find(_tasks, id, &TaskDef::id);
    return task != _tasks.end() ? &*task : nullptr;
}

TaskDef const* Config::firstTaskFor(AppliesTo item) const noexcept
{
    auto const task = std::ranges::find(_tasks, item, &TaskDef::appliesTo);
    return task != _tasks.end() ? &*task : nullptr;
}

WorkerDef const* Config::findWorker(std::string_view id) const noexcept
{
    auto const worker = std::ranges::find(_workers, id, &WorkerDef::id);
    return worker != _workers.end() ? &*worker : nullptr;
}

WorkerDef const& Config::workerFor(std::string_view taskId) const noexcept
{
    auto const* const task = findTask(taskId);
    auto const* const worker = task != nullptr ? findWorker(task->worker) : nullptr;
    return worker != nullptr ? *worker : _workers.front();
}

bool takesNoProfileFor(TaskDef const& task, WorkerDef const& worker) noexcept
{
    return agentRowOf(worker.agent).settings == SettingsProfile::NotTaken && !task.permissions.empty();
}

} // namespace orchestrator
