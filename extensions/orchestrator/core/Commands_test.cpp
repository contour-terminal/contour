// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <orchestrator/core/Commands.hpp>
#include <orchestrator/core/Fixtures.hpp>

using namespace std::string_view_literals;

namespace
{

using namespace orchestrator;

/// Words no declared command may hold: each would let a command leave its root, act on another repository
/// or force a destructive operation. Matched case-sensitively, so git's `-c key=value` stays allowed.
constexpr auto Forbidden = std::array {
    "-C"sv, "--git-dir"sv, "--work-tree"sv, "-R"sv, "--force"sv, "--force-with-lease"sv, "--hard"sv, "-f"sv,
};

/// The one command allowed a `-f`, because its argv pins it to one fixed path.
constexpr auto ForceExempt = std::pair { "git-clean"sv, "-f"sv };

/// The commands that run git inside an agent-controlled worktree, and so read attributes from the empty tree
/// and take no optional lock.
constexpr auto CleanupCommands = std::array {
    "git-status"sv,      "git-head-reachable"sv, "git-operation-state"sv,
    "git-index-flags"sv, "git-clean"sv,          "git-worktree-remove"sv,
};

/// The environment every cleanup command, and only a cleanup command, carries.
constexpr auto CleanupOnlyEnvironment = std::array { "GIT_ATTR_SOURCE"sv, "GIT_OPTIONAL_LOCKS"sv };

/// Whether @p word could reach @p option: the option itself; for a long option, any abbreviation of it,
/// however short (git accepts every unambiguous prefix, so `--f` alone can mean `--force`), with or without
/// `=value`, the bare `--` separator excepted; for a short option, any bundle of single-letter flags holding
/// its letter.
constexpr bool spellsOption(std::string_view word, std::string_view option) noexcept
{
    if (word == option)
        return true;
    if (option.starts_with("--"))
    {
        auto const stem = word.substr(0, word.find('='));
        return stem.starts_with("--") && stem.size() > 2 && option.starts_with(stem);
    }
    return word.size() > 1 && word.starts_with('-') && !word.starts_with("--")
           && word.substr(1).contains(option[1]);
}

enum class Spelled : uint8_t
{
    No,
    Yes,
};

struct SpellingRow
{
    std::string_view word;
    std::string_view option;
    Spelled expected;
};

constexpr auto SpellingRows = std::array {
    SpellingRow { .word = "-f", .option = "-f", .expected = Spelled::Yes },
    SpellingRow { .word = "-fdx", .option = "-f", .expected = Spelled::Yes },
    SpellingRow { .word = "-xf", .option = "-f", .expected = Spelled::Yes },
    SpellingRow { .word = "-c", .option = "-C", .expected = Spelled::No },
    SpellingRow { .word = "-b", .option = "-f", .expected = Spelled::No },
    SpellingRow { .word = "--", .option = "--force", .expected = Spelled::No },
    SpellingRow { .word = "--force", .option = "--force", .expected = Spelled::Yes },
    SpellingRow { .word = "--forc", .option = "--force", .expected = Spelled::Yes },
    SpellingRow { .word = "--for", .option = "--force", .expected = Spelled::Yes },
    SpellingRow { .word = "--fo", .option = "--force", .expected = Spelled::Yes },
    SpellingRow { .word = "--f", .option = "--force", .expected = Spelled::Yes },
    SpellingRow { .word = "--f=x", .option = "--force", .expected = Spelled::Yes },
    SpellingRow { .word = "--h", .option = "--hard", .expected = Spelled::Yes },
    SpellingRow { .word = "--ha", .option = "--hard", .expected = Spelled::Yes },
    SpellingRow { .word = "--g", .option = "--git-dir", .expected = Spelled::Yes },
    SpellingRow { .word = "--w", .option = "--work-tree", .expected = Spelled::Yes },
    SpellingRow { .word = "--=x", .option = "--force", .expected = Spelled::No },
    SpellingRow { .word = "--force-with", .option = "--force-with-lease", .expected = Spelled::Yes },
    SpellingRow {
        .word = "--force-with-lease=main", .option = "--force-with-lease", .expected = Spelled::Yes },
    SpellingRow { .word = "--har", .option = "--hard", .expected = Spelled::Yes },
    SpellingRow { .word = "--git-dir=/x", .option = "--git-dir", .expected = Spelled::Yes },
    SpellingRow { .word = "--forced", .option = "--force", .expected = Spelled::No },
    SpellingRow { .word = "--porcelain", .option = "--force", .expected = Spelled::No },
    SpellingRow { .word = "-f", .option = "--force", .expected = Spelled::No },
};

[[nodiscard]] bool hasEnv(CommandDecl const& decl, std::string_view name)
{
    return std::ranges::contains(decl.environment, name, &EnvironmentVariable::name);
}

[[nodiscard]] std::optional<std::string_view> envValue(std::span<EnvironmentVariable const> environment,
                                                       std::string_view name)
{
    auto const found = std::ranges::find(environment, name, &EnvironmentVariable::name);
    if (found == environment.end())
        return std::nullopt;
    return found->value;
}

// {{{ substitute rows

enum class Outcome : uint8_t
{
    Substituted,
    UnknownPlaceholder,
    MissingArgument,
    Rejected,
};

[[nodiscard]] Outcome outcomeOf(SubstituteError error) noexcept
{
    switch (error)
    {
        case SubstituteError::UnknownPlaceholder: return Outcome::UnknownPlaceholder;
        case SubstituteError::MissingArgument: return Outcome::MissingArgument;
        case SubstituteError::Rejected: return Outcome::Rejected;
    }
    return Outcome::Rejected;
}

constexpr auto IssuesArgv = std::array { "gh"sv, "issue"sv, "list"sv, "--search"sv, "{query}"sv };
constexpr auto FetchArgv = std::array { "git"sv, "fetch"sv, "--quiet"sv, "{remote}"sv };
constexpr auto UnknownArgv = std::array { "git"sv, "{nope}"sv };
constexpr auto PartialArgv = std::array { "git"sv, "x{remote}"sv };
constexpr auto IssuesSubstituted =
    std::array { "gh"sv, "issue"sv, "list"sv, "--search"sv, "assignee:@me is:open"sv };
constexpr auto FetchSubstituted = std::array { "git"sv, "fetch"sv, "--quiet"sv, "origin"sv };

struct SubstituteRow
{
    std::span<std::string_view const> argv;
    std::string_view argumentName; ///< Empty: the call passes no argument.
    std::string_view argumentValue;
    Outcome expected;
    std::span<std::string_view const> result; ///< The argv when expected is Substituted.
};

constexpr auto SubstituteRows = std::array {
    SubstituteRow { .argv = IssuesArgv,
                    .argumentName = "query",
                    .argumentValue = "-label:x",
                    .expected = Outcome::Rejected,
                    .result = {} },
    SubstituteRow { .argv = FetchArgv,
                    .argumentName = "remote",
                    .argumentValue = "ext::sh -c x",
                    .expected = Outcome::Rejected,
                    .result = {} },
    SubstituteRow { .argv = IssuesArgv,
                    .argumentName = "query",
                    .argumentValue = "assignee:@me is:open",
                    .expected = Outcome::Substituted,
                    .result = IssuesSubstituted },
    SubstituteRow { .argv = FetchArgv,
                    .argumentName = "remote",
                    .argumentValue = "origin",
                    .expected = Outcome::Substituted,
                    .result = FetchSubstituted },
    SubstituteRow { .argv = FetchArgv,
                    .argumentName = "",
                    .argumentValue = "",
                    .expected = Outcome::MissingArgument,
                    .result = {} },
    SubstituteRow { .argv = FetchArgv,
                    .argumentName = "query",
                    .argumentValue = "origin",
                    .expected = Outcome::MissingArgument,
                    .result = {} },
    SubstituteRow { .argv = UnknownArgv,
                    .argumentName = "nope",
                    .argumentValue = "x",
                    .expected = Outcome::UnknownPlaceholder,
                    .result = {} },
    SubstituteRow { .argv = PartialArgv,
                    .argumentName = "remote",
                    .argumentValue = "origin",
                    .expected = Outcome::UnknownPlaceholder,
                    .result = {} },
};

// }}}
// {{{ the manifest-v2 line-grammar checker

/// Why checkManifest() refused a manifest; None when it accepted it.
enum class Fault : uint8_t
{
    None,
    UnknownLine,           ///< An unknown keyword, or a known one in the wrong shape.
    MissingHeader,         ///< No `name` or no `version` line, or a line before `version`.
    EmptyValue,            ///< A line that takes a value, given none.
    VersionMismatch,       ///< A version other than 1 or 2, or a version-2 line in a version-1 manifest.
    QuotedWord,            ///< An argv word holding a quote: an attempt at a word with a space.
    BadPlaceholder,        ///< A brace that is not part of a whole-word `{name}`.
    UndeclaredPlaceholder, ///< A `{name}` without an `arg name` line.
    MissingRoot,           ///< A command or pane bound to no `@root`.
    UndeclaredRoot,        ///< `@root`, `read root` or `path:root` naming no `root` line.
    UndeclaredTarget,      ///< `command-env-for` or `pane-env` naming no such command or pane.
    UnknownKind,           ///< An `arg` of no known kind.
    EmptyArgv,             ///< A command or pane without a word to run.
    PlaceholderProgram,    ///< A command or pane whose program is a placeholder.
    Duplicate,             ///< The same thing declared twice.
};

enum class Valued : uint8_t
{
    No,  ///< The line has no ` = value` part.
    Yes, ///< The line is `head = value`.
};

/// One line kind: its keyword, how many words precede the `=`, how many of them name it (the duplicate
/// key), whether it has a value, and the first manifest version that knows it.
struct LineKind
{
    std::string_view keyword;
    std::size_t headWords;
    std::size_t keyWords;
    Valued valued;
    unsigned since;
    std::size_t rootAt; ///< The head word that is `@root`; 0 for a line bound to no root.
};

constexpr auto LineKinds = std::array {
    LineKind {
        .keyword = "name", .headWords = 1, .keyWords = 1, .valued = Valued::Yes, .since = 1, .rootAt = 0 },
    LineKind {
        .keyword = "version", .headWords = 1, .keyWords = 1, .valued = Valued::Yes, .since = 1, .rootAt = 0 },
    LineKind { .keyword = "placement",
               .headWords = 2,
               .keyWords = 1,
               .valued = Valued::No,
               .since = 2,
               .rootAt = 0 },
    LineKind {
        .keyword = "root", .headWords = 2, .keyWords = 2, .valued = Valued::Yes, .since = 2, .rootAt = 0 },
    LineKind {
        .keyword = "read", .headWords = 2, .keyWords = 2, .valued = Valued::No, .since = 2, .rootAt = 0 },
    LineKind {
        .keyword = "arg", .headWords = 2, .keyWords = 2, .valued = Valued::Yes, .since = 2, .rootAt = 0 },
    LineKind { .keyword = "command-env",
               .headWords = 2,
               .keyWords = 2,
               .valued = Valued::Yes,
               .since = 2,
               .rootAt = 0 },
    LineKind {
        .keyword = "command", .headWords = 4, .keyWords = 2, .valued = Valued::Yes, .since = 2, .rootAt = 3 },
    LineKind { .keyword = "command-env-for",
               .headWords = 3,
               .keyWords = 3,
               .valued = Valued::Yes,
               .since = 2,
               .rootAt = 0 },
    LineKind {
        .keyword = "pane", .headWords = 3, .keyWords = 2, .valued = Valued::Yes, .since = 2, .rootAt = 2 },
    LineKind { .keyword = "pane-env",
               .headWords = 3,
               .keyWords = 3,
               .valued = Valued::Yes,
               .since = 2,
               .rootAt = 0 },
    LineKind {
        .keyword = "inbox", .headWords = 2, .keyWords = 2, .valued = Valued::Yes, .since = 2, .rootAt = 0 },
    LineKind {
        .keyword = "serve", .headWords = 2, .keyWords = 2, .valued = Valued::No, .since = 2, .rootAt = 0 },
    LineKind {
        .keyword = "action", .headWords = 2, .keyWords = 2, .valued = Valued::Yes, .since = 2, .rootAt = 0 },
};

constexpr auto PlainKinds = std::array { "number"sv, "uuid"sv, "name"sv, "ref"sv, "text"sv };

[[nodiscard]] std::vector<std::string_view> wordsOf(std::string_view text)
{
    auto words = std::vector<std::string_view> {};
    for (auto const word: std::views::split(text, ' '))
        words.emplace_back(word.begin(), word.end());
    return words;
}

/// Checks an argv for quoted words and braces that are not a declared whole-word placeholder.
[[nodiscard]] Fault checkArgv(std::string_view argv, std::set<std::string, std::less<>> const& declared)
{
    auto const words = wordsOf(argv);
    if (words.empty() || (words.size() == 1 && words.front().empty()))
        return Fault::EmptyArgv;
    if (placeholderName(words.front()))
        return Fault::PlaceholderProgram;
    for (auto const word: words)
    {
        if (word.empty())
            return Fault::UnknownLine;
        if (word.contains('"') || word.contains('\''))
            return Fault::QuotedWord;
        if (!word.contains('{') && !word.contains('}'))
            continue;
        auto const name = placeholderName(word);
        if (!name)
            return Fault::BadPlaceholder;
        if (!declared.contains("arg " + std::string(*name)))
            return Fault::UndeclaredPlaceholder;
    }
    return Fault::None;
}

/// What the lines read so far have declared.
struct ManifestState
{
    std::set<std::string, std::less<>> declared; ///< "<keyword> <name>…" of every declaration.
    unsigned version = 0;                        ///< 0 until a valid `version` line.

    [[nodiscard]] bool isDeclared(std::string_view keyword, std::string_view name) const
    {
        return declared.contains(std::string(keyword) + ' ' + std::string(name));
    }
};

/// One line split at its ` = `.
struct ManifestLine
{
    LineKind const* kind;
    std::vector<std::string_view> head;
    std::string_view value;
};

/// Checks what a line refers to: roots, arguments, commands, panes and kinds.
[[nodiscard]] Fault checkReferences(ManifestLine const& line, ManifestState const& state)
{
    auto const keyword = line.kind->keyword;
    auto const& head = line.head;
    if (line.kind->rootAt != 0 && !state.isDeclared("root", head[line.kind->rootAt].substr(1)))
        return Fault::UndeclaredRoot;
    if (keyword == "read" && !state.isDeclared("root", head[1]))
        return Fault::UndeclaredRoot;
    if (keyword == "root" && !line.value.starts_with("config:"))
        return Fault::UnknownLine;
    if (keyword == "command" && head[2] != "short" && head[2] != "long")
        return Fault::UnknownLine;
    if (keyword == "arg" && line.value.starts_with("path:"))
        return state.isDeclared("root", line.value.substr(5)) ? Fault::None : Fault::UndeclaredRoot;
    if (keyword == "arg" && !std::ranges::contains(PlainKinds, line.value))
        return Fault::UnknownKind;
    if ((keyword == "command-env-for" && !state.isDeclared("command", head[1]))
        || (keyword == "pane-env" && !state.isDeclared("pane", head[1])))
        return Fault::UndeclaredTarget;
    if (keyword == "command" || keyword == "pane")
        return checkArgv(line.value, state.declared);
    return Fault::None;
}

/// Checks a line's shape and records what it declares.
[[nodiscard]] Fault checkLine(std::string_view text, ManifestState& state)
{
    // A line ending in " =" has an empty value.
    auto const equals = text.ends_with(" =") && !text.contains(" = ") ? text.size() - 2 : text.find(" = ");
    auto head = wordsOf(text.substr(0, equals));
    if (head.empty() || head.front().empty())
        return Fault::UnknownLine;
    auto const* const kind = std::ranges::find(LineKinds, head.front(), &LineKind::keyword);
    auto const line = ManifestLine {
        .kind = kind,
        .head = std::move(head),
        .value = equals == std::string_view::npos ? ""sv : text.substr(std::min(equals + 3, text.size())),
    };
    if (line.kind == LineKinds.end())
        return Fault::UnknownLine;
    auto const keyword = line.kind->keyword;
    auto const rootAt = line.kind->rootAt;
    if (rootAt != 0 && (line.head.size() <= rootAt || !line.head[rootAt].starts_with('@')))
        return Fault::MissingRoot;
    if (line.head.size() != line.kind->headWords
        || (equals != std::string_view::npos) != (line.kind->valued == Valued::Yes))
        return Fault::UnknownLine;
    if (keyword != "name" && keyword != "version" && state.version == 0)
        return Fault::MissingHeader;
    if (state.version != 0 && line.kind->since > state.version)
        return Fault::VersionMismatch;
    auto key = std::string(keyword);
    for (auto const word: std::span(line.head).subspan(1, line.kind->keyWords - 1))
        key += ' ' + std::string(word);
    if (!state.declared.insert(key).second)
        return Fault::Duplicate;
    // An empty argv is refused by the argv check, which names the fault more precisely.
    if (line.kind->valued == Valued::Yes && line.value.empty() && line.kind->rootAt == 0)
        return Fault::EmptyValue;
    if (keyword == "version")
    {
        state.version = line.value == "2" ? 2U : 0U;
        if (line.value == "1")
            state.version = 1U;
        return state.version == 0 ? Fault::VersionMismatch : Fault::None;
    }
    return checkReferences(line, state);
}

/// The line-grammar half of manifest version 2, independent of the tables it is checked against: blank and
/// '#' lines are skipped, every other line is checked in order, and a name must be declared before use.
[[nodiscard]] Fault checkManifest(std::string_view text)
{
    auto state = ManifestState {};
    for (auto const rawLine: std::views::split(text, '\n'))
    {
        auto const line = std::string_view(rawLine.begin(), rawLine.end());
        if (line.empty() || line.starts_with('#'))
            continue;
        if (auto const fault = checkLine(line, state); fault != Fault::None)
            return fault;
    }
    return state.declared.contains("name") && state.version != 0 ? Fault::None : Fault::MissingHeader;
}

/// One file of the corpus and the verdict it pins.
struct CorpusRow
{
    std::string_view file;
    Fault expected;
};

constexpr auto CorpusRows = std::array {
    CorpusRow { .file = "valid/action.manifest", .expected = Fault::None },
    CorpusRow { .file = "valid/arg.manifest", .expected = Fault::None },
    CorpusRow { .file = "valid/command-env-for.manifest", .expected = Fault::None },
    CorpusRow { .file = "valid/command-env.manifest", .expected = Fault::None },
    CorpusRow { .file = "valid/command.manifest", .expected = Fault::None },
    CorpusRow { .file = "valid/inbox.manifest", .expected = Fault::None },
    CorpusRow { .file = "valid/orchestrator.manifest", .expected = Fault::None },
    CorpusRow { .file = "valid/pane-env.manifest", .expected = Fault::None },
    CorpusRow { .file = "valid/pane.manifest", .expected = Fault::None },
    CorpusRow { .file = "valid/placement.manifest", .expected = Fault::None },
    CorpusRow { .file = "valid/read.manifest", .expected = Fault::None },
    CorpusRow { .file = "valid/root.manifest", .expected = Fault::None },
    CorpusRow { .file = "valid/serve.manifest", .expected = Fault::None },
    CorpusRow { .file = "invalid/brace-inside-word.manifest", .expected = Fault::BadPlaceholder },
    CorpusRow { .file = "invalid/command-without-root.manifest", .expected = Fault::MissingRoot },
    CorpusRow { .file = "invalid/duplicate-command.manifest", .expected = Fault::Duplicate },
    CorpusRow { .file = "invalid/empty-argv.manifest", .expected = Fault::EmptyArgv },
    CorpusRow { .file = "invalid/empty-head.manifest", .expected = Fault::UnknownLine },
    CorpusRow { .file = "invalid/empty-name.manifest", .expected = Fault::EmptyValue },
    CorpusRow { .file = "invalid/env-for-undeclared-command.manifest", .expected = Fault::UndeclaredTarget },
    CorpusRow { .file = "invalid/path-of-undeclared-root.manifest", .expected = Fault::UndeclaredRoot },
    CorpusRow { .file = "invalid/placeholder-program.manifest", .expected = Fault::PlaceholderProgram },
    CorpusRow { .file = "invalid/quoted-word.manifest", .expected = Fault::QuotedWord },
    CorpusRow { .file = "invalid/read-undeclared-root.manifest", .expected = Fault::UndeclaredRoot },
    CorpusRow { .file = "invalid/root-out-of-place.manifest", .expected = Fault::MissingRoot },
    CorpusRow { .file = "invalid/undeclared-placeholder.manifest", .expected = Fault::UndeclaredPlaceholder },
    CorpusRow { .file = "invalid/unknown-kind.manifest", .expected = Fault::UnknownKind },
    CorpusRow { .file = "invalid/unknown-line.manifest", .expected = Fault::UnknownLine },
    CorpusRow { .file = "invalid/version-1-with-v2-line.manifest", .expected = Fault::VersionMismatch },
};

// }}}

/// Every argv the tables declare, with the name of its command or pane.
[[nodiscard]] std::vector<std::pair<std::string_view, std::span<std::string_view const>>> everyArgv()
{
    auto all = std::vector<std::pair<std::string_view, std::span<std::string_view const>>> {};
    for (auto const& decl: commands())
        all.emplace_back(decl.name, decl.argv);
    for (auto const& decl: paneTemplates())
        all.emplace_back(decl.name, decl.argv);
    return all;
}

} // namespace

TEST_CASE("a forbidden option is recognised however it is spelled", "[orchestrator][commands]")
{
    for (auto const& row: SpellingRows)
    {
        CAPTURE(row.word, row.option, row.expected);
        CHECK((spellsOption(row.word, row.option) ? Spelled::Yes : Spelled::No) == row.expected);
    }
}

TEST_CASE("every declared command obeys the grammar and forces nothing", "[orchestrator][commands]")
{
    for (auto const& decl: commands())
    {
        CAPTURE(decl.name);
        for (auto const word: decl.argv)
        {
            CAPTURE(word);
            CHECK(!word.empty());
            CHECK(!word.contains(' '));
            auto const exempt = decl.name == ForceExempt.first && word == ForceExempt.second;
            CHECK((exempt || std::ranges::none_of(Forbidden, [&](auto option) {
                       return spellsOption(word, option);
                   })));
        }
    }
    // git-clean is the one -f in the table, pinned to one fixed path.
    auto const* const clean = findCommand("git-clean");
    REQUIRE(clean != nullptr);
    CHECK(std::ranges::equal(clean->argv, std::array { "git"sv, "clean"sv, "-f"sv, "--"sv, "STATUS.md"sv }));
}

TEST_CASE("GIT_ATTR_SOURCE and GIT_OPTIONAL_LOCKS are on cleanup commands only",
          "[orchestrator][commands][env-for-cleanup-only]")
{
    for (auto const& decl: commands())
        for (auto const name: CleanupOnlyEnvironment)
        {
            CAPTURE(decl.name, name);
            CHECK(hasEnv(decl, name) == std::ranges::contains(CleanupCommands, decl.name));
        }
    for (auto const name: CleanupCommands)
    {
        CAPTURE(name);
        auto const* const decl = findCommand(name);
        REQUIRE(decl != nullptr);
        CHECK(envValue(decl->environment, "GIT_OPTIONAL_LOCKS") == "0");
    }
}

TEST_CASE("the cleanup checks ask git for no more than they read", "[orchestrator][commands]")
{
    struct Row
    {
        std::string_view command;
        std::span<std::string_view const> argv;
    };
    static constexpr auto Reachable = std::array {
        "git"sv,  "for-each-ref"sv, "--count=1"sv, "--contains"sv,
        "HEAD"sv, "refs/heads"sv,   "refs/tags"sv, "refs/remotes"sv,
    };
    static constexpr auto IndexFlags = std::array { "git"sv, "ls-files"sv, "-v"sv };
    static constexpr auto Rows = std::array {
        Row { .command = "git-head-reachable", .argv = Reachable },
        Row { .command = "git-index-flags", .argv = IndexFlags },
    };
    for (auto const& row: Rows)
    {
        CAPTURE(row.command);
        auto const* const decl = findCommand(row.command);
        REQUIRE(decl != nullptr);
        CHECK(std::ranges::equal(decl->argv, row.argv));
        CHECK(decl->root == Root::Worktrees);
        CHECK(decl->timeout == Timeout::Short);
    }
}

TEST_CASE("the operation-state check reads git's untranslated, uncoloured long status",
          "[orchestrator][commands]")
{
    auto const* const decl = findCommand("git-operation-state");
    REQUIRE(decl != nullptr);
    CHECK(std::ranges::equal(decl->argv,
                             std::array { "git"sv,
                                          "-c"sv,
                                          "status.displayCommentPrefix=false"sv,
                                          "-c"sv,
                                          "color.status=false"sv,
                                          "status"sv,
                                          "--long"sv }));
    CHECK(envValue(decl->environment, "LC_ALL") == "C");
    CHECK(decl->root == Root::Worktrees);
    CHECK(decl->timeout == Timeout::Short);
}

TEST_CASE("an issue branch never tracks the default branch", "[orchestrator][commands]")
{
    auto const* const add = findCommand("git-worktree-add-branch");
    REQUIRE(add != nullptr);
    CHECK(std::ranges::contains(add->argv, "--no-track"sv));
    CHECK(add->root == Root::Repos);

    auto const* const agent = findPaneTemplate("agent");
    REQUIRE(agent != nullptr);
    CHECK(envValue(agent->environment, "GIT_CONFIG_COUNT") == "2");
    CHECK(envValue(agent->environment, "GIT_CONFIG_KEY_0") == "push.default");
    CHECK(envValue(agent->environment, "GIT_CONFIG_VALUE_0") == "upstream");
    CHECK(envValue(agent->environment, "GIT_CONFIG_KEY_1") == "push.autoSetupRemote");
    CHECK(envValue(agent->environment, "GIT_CONFIG_VALUE_1") == "true");
    // No permission flag and no --settings: exactly these words.
    CHECK(std::ranges::equal(
        agent->argv,
        std::array { "claude"sv, "--model"sv, "{model}"sv, "--session-id"sv, "{session}"sv, "{prompt}"sv }));
}

TEST_CASE("git configuration is set only where it is declared", "[orchestrator][commands]")
{
    // A command's own environment never sets GIT_CONFIG_*: it would replace commandEnvironment()'s, and with
    // it the switched-off core.fsmonitor and core.hooksPath.
    for (auto const& decl: commands())
    {
        CAPTURE(decl.name);
        CHECK(std::ranges::none_of(
            decl.environment, [](auto const& variable) { return variable.name.starts_with("GIT_CONFIG_"); }));
    }
    // The pane's GIT_CONFIG_* variables are exactly its declared pairs, counted by GIT_CONFIG_COUNT.
    static constexpr auto PanePairs = std::array {
        EnvironmentVariable { .name = "push.default", .value = "upstream" },
        EnvironmentVariable { .name = "push.autoSetupRemote", .value = "true" },
    };
    auto const* const agent = findPaneTemplate("agent");
    REQUIRE(agent != nullptr);
    auto const gitConfig = static_cast<std::size_t>(std::ranges::count_if(
        agent->environment, [](auto const& variable) { return variable.name.starts_with("GIT_CONFIG_"); }));
    CHECK(gitConfig == (2 * PanePairs.size()) + 1);
    CHECK(envValue(agent->environment, "GIT_CONFIG_COUNT") == std::to_string(PanePairs.size()));
    for (auto const index: std::views::iota(std::size_t { 0 }, PanePairs.size()))
    {
        CAPTURE(index);
        CHECK(envValue(agent->environment, "GIT_CONFIG_KEY_" + std::to_string(index))
              == PanePairs[index].name);
        CHECK(envValue(agent->environment, "GIT_CONFIG_VALUE_" + std::to_string(index))
              == PanePairs[index].value);
    }
}

TEST_CASE("names are unique and every placeholder is a declared argument", "[orchestrator][commands]")
{
    auto names = std::set<std::string_view> {};
    auto used = std::set<std::string_view> {};
    for (auto const& [name, argv]: everyArgv())
    {
        CAPTURE(name);
        CHECK(names.insert(name).second);
        for (auto const word: argv)
        {
            CAPTURE(word);
            if (!word.contains('{') && !word.contains('}'))
                continue;
            auto const placeholder = placeholderName(word);
            REQUIRE(placeholder.has_value());
            CHECK(findArgumentDecl(*placeholder) != nullptr);
            used.insert(*placeholder);
        }
    }
    for (auto const& decl: argumentDecls())
    {
        CAPTURE(decl.name);
        CHECK(used.contains(decl.name));
        CHECK(decl.root.has_value() == (decl.kind == ArgumentKind::Path));
    }
}

TEST_CASE("substitute checks each value and keeps it one word", "[orchestrator][commands][substitute]")
{
    for (auto const& row: SubstituteRows)
    {
        CAPTURE(row.argv, row.argumentName, row.argumentValue);
        auto arguments = std::vector<Argument> {};
        if (!row.argumentName.empty())
            arguments.push_back(
                Argument { .name = row.argumentName, .value = std::string(row.argumentValue) });
        auto const result = substitute(row.argv, arguments);
        CHECK((result ? Outcome::Substituted : outcomeOf(result.error())) == row.expected);
        if (result)
            CHECK(std::ranges::equal(*result, row.result));
    }
}

TEST_CASE("manifestLines spells the declared tables", "[orchestrator][commands][manifest]")
{
    static constexpr auto Expected = std::array {
        "command git-status short @worktrees = git status --porcelain --untracked-files=normal --ignore-submodules=none"sv,
        "pane agent @worktrees = claude --model {model} --session-id {session} {prompt}"sv,
        "command-env-for git-status GIT_ATTR_SOURCE = 4b825dc642cb6eb9a060e54bf8d69288fbee4904"sv,
        "command-env-for git-status GIT_OPTIONAL_LOCKS = 0"sv,
        "command git-head-reachable short @worktrees = git for-each-ref --count=1 --contains HEAD refs/heads refs/tags refs/remotes"sv,
        "command git-index-flags short @worktrees = git ls-files -v"sv,
        "command-env-for git-operation-state LC_ALL = C"sv,
        "command git-worktree-remove long @worktrees = git worktree remove {path}"sv,
        "arg path = path:worktrees"sv,
        "root repos = config:repos/*/path"sv,
    };
    auto const lines = manifestLines();
    for (auto const line: Expected)
    {
        CAPTURE(line);
        CHECK(std::ranges::contains(lines, line));
    }
}

TEST_CASE("the manifest-v2 corpus", "[orchestrator][commands][manifest][corpus]")
{
    SECTION("each file gets the verdict it pins")
    {
        for (auto const& row: CorpusRows)
        {
            CAPTURE(row.file, row.expected);
            CHECK(checkManifest(readFixture(FixtureRoot::Contract, "manifest-v2/" + std::string(row.file)))
                  == row.expected);
        }
    }
    SECTION("every file in the corpus has a row")
    {
        auto listed = std::vector<std::string> {};
        for (auto const directory: std::array { "valid"sv, "invalid"sv })
            for (auto const& name:
                 fixtureNames(FixtureRoot::Contract, "manifest-v2/" + std::string(directory)))
                listed.push_back(std::string(directory) + '/' + name);
        CHECK(listed.size() == CorpusRows.size());
        for (auto const& name: listed)
        {
            CAPTURE(name);
            CHECK(std::ranges::contains(CorpusRows, std::string_view(name), &CorpusRow::file));
        }
    }
    SECTION("the declared tables reproduce the golden manifest")
    {
        auto text = std::string("name = orchestrator\nversion = 2\nplacement daemon\n");
        for (auto const& line: manifestLines())
            text += line + '\n';
        CHECK(text == readFixture(FixtureRoot::Contract, "manifest-v2/valid/orchestrator.manifest"));
    }
}
