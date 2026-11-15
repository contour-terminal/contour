// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/core/Config.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
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
#include <orchestrator/core/Json.hpp>
#include <orchestrator/core/Model.hpp>
#include <orchestrator/core/Provider.hpp>

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
                                              "finished_retention",
                                              &T::finishedRetention,
                                              "repos",
                                              &T::repos,
                                              "tasks",
                                              &T::tasks);
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
                                              &T::prompt);
};

namespace orchestrator
{

namespace
{
    using config::detail::JsonConfig;
    using config::detail::JsonDuration;
    using config::detail::JsonIssueSource;
    using config::detail::JsonRepo;
    using config::detail::JsonTask;

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
    };

    /// The issue sources this version reads; Jira comes later.
    constexpr auto IssueSources = std::array { "forge"sv };

    /// @return The meaning of @p text in @p table, or nothing if no row spells it.
    template <typename Meaning, std::size_t N>
    constexpr std::optional<Meaning> meaningOf(std::array<Spelling<Meaning>, N> const& table,
                                               std::string_view text) noexcept
    {
        auto const row = std::ranges::find(table, text, &Spelling<Meaning>::text);
        return row != table.end() ? std::optional { row->meaning } : std::nullopt;
    }

    /// One of the dashboard's own keys, which no task may take; the arrows are no characters.
    struct ReservedKey
    {
        char key;
        std::string_view name;
    };

    constexpr auto ReservedKeys = std::array {
        ReservedKey { .key = '\r', .name = "Enter"sv }, ReservedKey { .key = '\n', .name = "newline"sv },
        ReservedKey { .key = '\t', .name = "Tab"sv },   ReservedKey { .key = 'd', .name = "d"sv },
        ReservedKey { .key = 'e', .name = "e"sv },      ReservedKey { .key = 'b', .name = "b"sv },
        ReservedKey { .key = 't', .name = "t"sv },      ReservedKey { .key = 'x', .name = "x"sv },
        ReservedKey { .key = '/', .name = "/"sv },      ReservedKey { .key = '?', .name = "?"sv },
    };

    /// The accepted values a problem's message lists, taken from the table that decides them.
    enum class Listing : uint8_t
    {
        None,
        Forges,       ///< ForgeSpellings that have a provider row.
        IssueSources, ///< IssueSources.
        ItemKinds,    ///< AppliesToSpellings.
        ReservedKeys, ///< ReservedKeys.
    };

    /// What each problem means; one row per ConfigProblem, in its order.
    struct ProblemText
    {
        ConfigProblem problem;
        std::string_view text;
        Listing listing = Listing::None;
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
        ProblemText { .problem = ConfigProblem::MissingUrl, .text = "must contain {url}" },
    };

    static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, ProblemTexts.size()),
                                      [](std::size_t i) {
                                          return static_cast<std::size_t>(ProblemTexts[i].problem) == i;
                                      }),
                  "ProblemTexts has one row per ConfigProblem, in its order");
    static_assert(ProblemTexts.size() == static_cast<std::size_t>(ConfigProblem::MissingUrl) + 1,
                  "ProblemTexts covers every ConfigProblem");

    /// @return The values @p listing names, as the user writes them.
    [[nodiscard]] std::vector<std::string_view> listed(Listing listing)
    {
        auto values = std::vector<std::string_view> {};
        switch (listing)
        {
            case Listing::None: break;
            case Listing::Forges:
                for (auto const& row: ForgeSpellings)
                    if (findProvider(row.forge) != nullptr)
                        values.push_back(row.name);
                break;
            case Listing::IssueSources: values.assign(IssueSources.begin(), IssueSources.end()); break;
            case Listing::ItemKinds:
                for (auto const& row: AppliesToSpellings)
                    values.push_back(row.text);
                break;
            case Listing::ReservedKeys:
                for (auto const& row: ReservedKeys)
                    values.push_back(row.name);
                break;
        }
        return values;
    }

    /// The remote a repository uses unless it names another.
    constexpr auto DefaultRemote = "origin"sv;
    /// What a prompt writes where the item's URL goes.
    constexpr auto UrlPlaceholder = "{url}"sv;
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

    /// @return Whether @p path lies strictly inside the directory @p root.
    [[nodiscard]] constexpr bool isInside(std::string_view path, std::string_view root) noexcept
    {
        return path.size() > root.size() && path.starts_with(root) && path[root.size()] == '/';
    }

    /// The JSON types a configuration value may have; null is accepted everywhere and means absent.
    enum class JsonType : uint8_t
    {
        String,   ///< A string.
        Count,    ///< A non-negative integer that fits 32 bits.
        Duration, ///< A string, or a non-negative integer (only 0 is accepted later).
        Strings,  ///< An array of strings.
        Objects,  ///< An array of objects of Field::object's kind.
    };

    /// The objects the configuration nests.
    enum class ObjectKind : uint8_t
    {
        Config,
        Repo,
        IssueSource,
        Task,
    };

    /// One key an object may have, and the type of its value.
    struct Field
    {
        std::string_view key;
        JsonType type;
        ObjectKind object = ObjectKind::Config; ///< For Objects: the kind of its elements.
    };

    constexpr auto ConfigFields = std::array {
        Field { .key = "worktree_root", .type = JsonType::String },
        Field { .key = "models", .type = JsonType::Strings },
        Field { .key = "max_agents", .type = JsonType::Count },
        Field { .key = "max_agents_per_repo", .type = JsonType::Count },
        Field { .key = "poll_every", .type = JsonType::Duration },
        Field { .key = "status_board_after", .type = JsonType::Duration },
        Field { .key = "finished_retention", .type = JsonType::Duration },
        Field { .key = "repos", .type = JsonType::Objects, .object = ObjectKind::Repo },
        Field { .key = "tasks", .type = JsonType::Objects, .object = ObjectKind::Task },
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
    };

    constexpr auto TaskFields = std::array {
        Field { .key = "id", .type = JsonType::String },
        Field { .key = "key", .type = JsonType::String },
        Field { .key = "label", .type = JsonType::String },
        Field { .key = "applies_to", .type = JsonType::String },
        Field { .key = "model", .type = JsonType::String },
        Field { .key = "prompt", .type = JsonType::String },
    };

    [[nodiscard]] constexpr std::span<Field const> fieldsOf(ObjectKind kind) noexcept
    {
        switch (kind)
        {
            case ObjectKind::Config: return ConfigFields;
            case ObjectKind::Repo: return RepoFields;
            case ObjectKind::IssueSource: return IssueSourceFields;
            case ObjectKind::Task: return TaskFields;
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

    /// Integers are read exactly, so that a count is never a rounded double.
    using Generic = glz::generic_u64;

    [[nodiscard]] ConfigError problemAt(std::string key, ConfigProblem problem)
    {
        return ConfigError { .key = std::move(key), .problem = problem };
    }

    [[nodiscard]] std::optional<ConfigError> checkObject(Generic const& value,
                                                         ObjectKind kind,
                                                         std::string const& key);

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
            case JsonType::Duration: return value.is_string() || number != nullptr ? std::nullopt : wrongType;
            case JsonType::Strings:
            case JsonType::Objects:
                if (array == nullptr)
                    return wrongType;
                for (auto const index: std::views::iota(std::size_t { 0 }, array->size()))
                {
                    auto const& element = (*array)[index];
                    auto const elementKey = std::format("{}[{}]", key, index);
                    if (field.type == JsonType::Strings && !element.is_string())
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

    /// @return The last component of the normalised, absolute @p path: its directory's name.
    [[nodiscard]] constexpr std::string_view lastComponent(std::string_view path) noexcept
    {
        return path.substr(path.rfind('/') + 1);
    }

    /// @return What is wrong with @p value as a Name, if anything.
    [[nodiscard]] constexpr std::optional<ConfigProblem> nameProblem(std::string_view value) noexcept
    {
        if (argumentAccepts(ArgumentKind::Name, value))
            return std::nullopt;
        return ConfigProblem::NotAName;
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

    /// @return What is wrong with @p value as a task key, if anything.
    [[nodiscard]] constexpr std::optional<ConfigProblem> keyProblem(std::string_view value) noexcept
    {
        if (value.size() == 1 && std::ranges::contains(ReservedKeys, value.front(), &ReservedKey::key))
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

    [[nodiscard]] Checked<std::string> issueQueryOf(JsonIssueSource const& json, std::string_view prefix)
    {
        auto const sourceKey = std::format("{}.source", prefix);
        if (!json.source)
            return fail(sourceKey, ConfigProblem::Missing);
        if (!std::ranges::contains(IssueSources, *json.source))
            return fail(sourceKey, ConfigProblem::UnsupportedSource);
        return checked(json.query, std::format("{}.query", prefix), textProblem);
    }

    /// @return The name a repository is known by: the one written at `name`, or else its directory's.
    [[nodiscard]] Checked<std::string> repoNameOf(std::optional<std::string> const& written,
                                                  std::string_view path,
                                                  std::string_view prefix,
                                                  std::span<Repo const> earlier)
    {
        if (written)
        {
            auto key = std::format("{}.name", prefix);
            if (auto const problem = nameProblem(*written))
                return fail(std::move(key), *problem);
            if (std::ranges::contains(earlier, *written, &Repo::name))
                return fail(std::move(key), ConfigProblem::Duplicate);
            return *written;
        }
        // Unwritten, the name is the directory's, and a problem with it is the path's.
        auto key = std::format("{}.path", prefix);
        auto const name = lastComponent(path);
        if (nameProblem(name))
            return fail(std::move(key), ConfigProblem::UnnamedDirectory);
        if (std::ranges::contains(earlier, name, &Repo::name))
            return fail(std::move(key), ConfigProblem::DuplicateDirectoryName);
        return std::string { name };
    }

    /// Validates the repository at @p index against those before it.
    [[nodiscard]] Checked<Repo> repoOf(JsonRepo const& json,
                                       std::size_t index,
                                       std::string_view path,
                                       std::string_view worktreeRoot,
                                       std::span<Repo const> earlier)
    {
        auto const prefix = std::format("repos[{}]", index);
        auto const key = [&](std::string_view field) {
            return std::format("{}.{}", prefix, field);
        };

        if (!json.path)
            return fail(key("path"), ConfigProblem::Missing);
        if (!argumentAccepts(ArgumentKind::Path, path))
            return fail(key("path"), ConfigProblem::NotAbsolutePath);
        if (path == worktreeRoot || std::ranges::contains(earlier, path, &Repo::path))
            return fail(key("path"), ConfigProblem::EqualRoots);
        // A worktree root inside a repository is fine: a directory belongs to its most specific root.
        if (isInside(path, worktreeRoot))
            return fail(key("path"), ConfigProblem::InsideWorktreeRoot);

        if (!json.forge)
            return fail(key("forge"), ConfigProblem::Missing);
        auto const forge = forgeNamed(*json.forge);
        if (!forge || findProvider(*forge) == nullptr)
            return fail(key("forge"), ConfigProblem::UnknownForge);

        auto const name = repoNameOf(json.name, path, prefix, earlier);
        if (!name)
            return std::unexpected(name.error());

        auto const remote = checked(std::optional { json.remote.value_or(std::string { DefaultRemote }) },
                                    key("remote"),
                                    nameProblem);
        if (!remote)
            return std::unexpected(remote.error());

        if (json.base && !argumentAccepts(ArgumentKind::Ref, *json.base))
            return fail(key("base"), ConfigProblem::NotARef);

        auto issueQueries = std::vector<std::string> {};
        for (auto const& source:
             json.issues ? std::span { *json.issues } : std::span<JsonIssueSource const> {})
        {
            auto query = issueQueryOf(source, std::format("{}.issues[{}]", prefix, issueQueries.size()));
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
                                                     HostInfo const& host)
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
            auto repo = repoOf((*value)[index], index, host.repoPaths[index], host.worktreeRoot, repos);
            if (!repo)
                return std::unexpected(repo.error());
            repos.push_back(std::move(*repo));
        }
        return repos;
    }

    /// Validates the task at @p index against those before it.
    [[nodiscard]] Checked<TaskDef> taskOf(JsonTask const& json,
                                          std::size_t index,
                                          std::span<std::string const> models,
                                          std::span<TaskDef const> earlier)
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

        auto const prompt = checked(json.prompt, key("prompt"), [](std::string_view value) {
            auto const problem = textProblem(value);
            if (!problem && !value.contains(UrlPlaceholder))
                return std::optional { ConfigProblem::MissingUrl };
            return problem;
        });
        if (!prompt)
            return std::unexpected(prompt.error());

        return TaskDef { .id = *id,
                         .key = keyText->front(),
                         .label = *label,
                         .appliesTo = *appliesTo,
                         .model = *model,
                         .prompt = *prompt };
    }

    [[nodiscard]] Checked<std::vector<TaskDef>> tasksOf(std::optional<std::vector<JsonTask>> const& value,
                                                        std::span<std::string const> models)
    {
        if (!value)
            return fail("tasks", ConfigProblem::Missing);
        if (value->empty())
            return fail("tasks", ConfigProblem::Empty);
        auto tasks = std::vector<TaskDef> {};
        for (auto const& json: *value)
        {
            auto task = taskOf(json, tasks.size(), models, tasks);
            if (!task)
                return std::unexpected(task.error());
            tasks.push_back(std::move(*task));
        }
        return tasks;
    }

} // namespace

std::string describe(ConfigProblem problem)
{
    auto const index = static_cast<std::size_t>(problem);
    if (index >= ProblemTexts.size())
        return {};
    auto const& row = ProblemTexts[index];
    auto text = std::string { row.text };
    auto separator = ": "sv;
    for (auto const value: listed(row.listing))
    {
        text += separator;
        text += value;
        separator = ", "sv;
    }
    return text;
}

std::string message(ConfigError const& error)
{
    if (error.key.empty())
        return describe(error.problem);
    return std::format("{}: {}", error.key, describe(error.problem));
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

std::expected<Config, ConfigError> Config::parse(std::string_view json, HostInfo const& host)
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
    if (!argumentAccepts(ArgumentKind::Path, host.worktreeRoot))
        return fail("worktree_root", ConfigProblem::NotAbsolutePath);

    auto values = Values { .worktreeRoot = host.worktreeRoot };
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
        if (auto e = assign(
                values.finishedRetention,
                durationOf(
                    document.finishedRetention, "finished_retention", DefaultFinishedRetention, NoMinimum)))
            return e;
        if (auto e = assign(values.repos, reposOf(document.repos, host)))
            return e;
        return assign(values.tasks, tasksOf(document.tasks, values.models));
    }();
    if (problem)
        return std::unexpected(*problem);
    return Config { std::move(values) };
}

Config::Config(Values values) noexcept:
    _worktreeRoot { std::move(values.worktreeRoot) },
    _models { std::move(values.models) },
    _maxAgents { values.maxAgents },
    _maxAgentsPerRepo { values.maxAgentsPerRepo },
    _pollEvery { values.pollEvery },
    _statusBoardAfter { values.statusBoardAfter },
    _finishedRetention { values.finishedRetention },
    _repos { std::move(values.repos) },
    _tasks { std::move(values.tasks) }
{
}

Repo const* Config::findRepo(std::string_view name) const noexcept
{
    auto const repo = std::ranges::find(_repos, name, &Repo::name);
    return repo != _repos.end() ? &*repo : nullptr;
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

} // namespace orchestrator
