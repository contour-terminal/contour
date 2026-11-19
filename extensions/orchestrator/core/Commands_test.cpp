// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
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
    "git-common-dir"sv,  "git-status"sv, "git-head-reachable"sv,  "git-operation-state"sv,
    "git-index-flags"sv, "git-clean"sv,  "git-worktree-remove"sv,
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

/// The environment @p decl runs with: commandEnvironment(), then its own, a later variable replacing an
/// earlier one of the same name, as the host applies them.
[[nodiscard]] std::map<std::string_view, std::string_view> effectiveEnvironment(CommandDecl const& decl)
{
    auto environment = std::map<std::string_view, std::string_view> {};
    for (auto const layer: std::array { commandEnvironment(), decl.environment })
        for (auto const& variable: layer)
            environment.insert_or_assign(variable.name, variable.value);
    return environment;
}

/// Which commands a forced git configuration pair must reach.
enum class Reach : uint8_t
{
    Every,   ///< Every declared command.
    Cleanup, ///< The cleanup commands, and no other.
};

struct ForcedPair
{
    std::string_view key;
    std::string_view value;
    Reach reach;
};

constexpr auto ForcedPairs = std::array {
    ForcedPair { .key = "core.fsmonitor", .value = "false", .reach = Reach::Every },
    ForcedPair { .key = "core.hooksPath", .value = "/dev/null", .reach = Reach::Every },
    ForcedPair { .key = "core.attributesFile", .value = "/dev/null", .reach = Reach::Cleanup },
};

/// The git configuration @p environment forces: GIT_CONFIG_KEY_n and GIT_CONFIG_VALUE_n for every n below
/// GIT_CONFIG_COUNT; std::nullopt when the count is no number or a pair it counts is missing.
[[nodiscard]] std::optional<std::vector<EnvironmentVariable>> forcedConfiguration(
    std::map<std::string_view, std::string_view> const& environment)
{
    auto const countText = environment.find("GIT_CONFIG_COUNT");
    if (countText == environment.end())
        return std::vector<EnvironmentVariable> {};
    auto count = std::size_t { 0 };
    auto const text = countText->second;
    auto const [end, error] = std::from_chars(text.data(), text.data() + text.size(), count);
    if (error != std::errc {} || end != text.data() + text.size())
        return std::nullopt;
    auto pairs = std::vector<EnvironmentVariable> {};
    for (auto const index: std::views::iota(std::size_t { 0 }, count))
    {
        auto const key = environment.find("GIT_CONFIG_KEY_" + std::to_string(index));
        auto const value = environment.find("GIT_CONFIG_VALUE_" + std::to_string(index));
        if (key == environment.end() || value == environment.end())
            return std::nullopt;
        pairs.push_back(EnvironmentVariable { .name = key->second, .value = value->second });
    }
    return pairs;
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
    UnknownLine,              ///< An unknown keyword, or a known one in the wrong shape.
    MissingHeader,            ///< No `name` or no `version` line, or a line before `version`.
    EmptyValue,               ///< A line that takes a value, given none.
    VersionMismatch,          ///< A version other than 1 or 2, or a version-2 line in a version-1 manifest.
    QuotedWord,               ///< An argv word holding a quote: an attempt at a word with a space.
    BadPlaceholder,           ///< A brace that is not part of a whole-word `{name}`.
    UndeclaredPlaceholder,    ///< A `{name}` without an `arg name` line.
    MissingRoot,              ///< A command or pane bound to no `@root`.
    UndeclaredRoot,           ///< `@root`, `read root` or `path:root` naming no `root` line.
    UndeclaredTarget,         ///< `command-env-for` or `pane-env` naming no such command or pane.
    UnknownKind,              ///< An `arg` of no known kind.
    EmptyArgv,                ///< A command or pane without a word to run.
    PlaceholderProgram,       ///< A command or pane whose program is a placeholder.
    Duplicate,                ///< The same thing declared twice.
    UnknownRootFlag,          ///< A word other than `create` between a root's label and its `=`.
    CreateOverList,           ///< `create` on a root whose pointer walks a list.
    MalformedRoot,            ///< A root pointer with an empty step, or a `*` that is not a whole step.
    MalformedEnvironmentName, ///< An environment name other than `[A-Za-z_][A-Za-z0-9_]*`.
    TabCharacter,             ///< A tab in a line: words are separated by single spaces only.
    SurroundingBlank,         ///< A line that begins or ends with a blank, which is never trimmed.
    ControlCharacter,         ///< A control character other than a tab, or a CR not right before the LF.
    UnsupportedLine,          ///< An `inbox` line: the kind is reserved, and no version supports it yet.
    BadTitle, ///< A `title` or `action` title longer than MaxTitleLength characters, or one that disguises
              ///< text.
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
    LineKind {
        .keyword = "title", .headWords = 1, .keyWords = 1, .valued = Valued::Yes, .since = 2, .rootAt = 0 },
};

/// The most characters (code points) a `title` line's value, or an `action` line's title, may hold.
constexpr auto MaxTitleLength = std::size_t { 40 };

/// A range of code points, both ends included.
struct CodePointRange
{
    char32_t first;
    char32_t last;
};

/// What a title may not hold: what renders as nothing or as something else, and what reorders or breaks the
/// text around it. The host refuses the same (vtextension's DisguisingCodePoints).
constexpr auto DisguisingCodePoints = std::array {
    CodePointRange { .first = 0x0000, .last = 0x001F },   // C0 controls
    CodePointRange { .first = 0x007F, .last = 0x009F },   // DEL, C1 controls
    CodePointRange { .first = 0x00AD, .last = 0x00AD },   // soft hyphen
    CodePointRange { .first = 0x061C, .last = 0x061C },   // Arabic letter mark
    CodePointRange { .first = 0x180E, .last = 0x180E },   // Mongolian vowel separator
    CodePointRange { .first = 0x200B, .last = 0x200F },   // zero-width space/joiners, LRM, RLM
    CodePointRange { .first = 0x2028, .last = 0x202E },   // line/paragraph separators, embeddings
    CodePointRange { .first = 0x2060, .last = 0x2064 },   // word joiner, invisible operators
    CodePointRange { .first = 0x2066, .last = 0x206F },   // isolates, deprecated format characters
    CodePointRange { .first = 0xFEFF, .last = 0xFEFF },   // zero-width no-break space
    CodePointRange { .first = 0xFFF9, .last = 0xFFFB },   // interlinear annotation
    CodePointRange { .first = 0xE0000, .last = 0xE007F }, // tag characters
};

/// The code points of @p text, which is UTF-8 (a test of this file's own corpus, so never malformed).
[[nodiscard]] std::vector<char32_t> codePointsOf(std::string_view text)
{
    auto codePoints = std::vector<char32_t> {};
    while (!text.empty())
    {
        auto const lead = static_cast<unsigned char>(text.front());
        // A lead byte's leading ones count its sequence's bytes; an ASCII byte has none and is one.
        auto const length = std::max(1U, static_cast<unsigned>(std::countl_one(lead)));
        auto codePoint = static_cast<char32_t>(length == 1 ? lead : lead & (0x7FU >> length));
        for (auto const byte: text.substr(1, length - 1))
            codePoint = (codePoint << 6U) | (static_cast<unsigned char>(byte) & 0x3FU);
        codePoints.push_back(codePoint);
        text.remove_prefix(std::min<std::size_t>(length, text.size()));
    }
    return codePoints;
}

/// Whether @p title is shown as written: at most MaxTitleLength code points, none of them disguising.
[[nodiscard]] bool isShownAsWritten(std::string_view title)
{
    auto const codePoints = codePointsOf(title);
    return codePoints.size() <= MaxTitleLength && std::ranges::none_of(codePoints, [](char32_t codePoint) {
               return std::ranges::any_of(DisguisingCodePoints, [codePoint](CodePointRange const& range) {
                   return codePoint >= range.first && codePoint <= range.last;
               });
           });
}

constexpr auto PlainKinds = std::array { "number"sv, "uuid"sv, "name"sv, "ref"sv, "text"sv };

/// The one word a `root` line may carry after its label: the host creates the directory if it is missing.
constexpr auto RootCreateWord = "create"sv;

[[nodiscard]] std::vector<std::string_view> wordsOfPath(std::string_view text)
{
    auto steps = std::vector<std::string_view> {};
    for (auto const step: std::views::split(text, '/'))
        steps.emplace_back(step.begin(), step.end());
    return steps;
}

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

/// Whether @p name is an environment name: `[A-Za-z_][A-Za-z0-9_]*`.
[[nodiscard]] bool isEnvironmentName(std::string_view name)
{
    auto const isWordChar = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    };
    return !name.empty() && (name.front() < '0' || name.front() > '9')
           && std::ranges::all_of(name, isWordChar);
}

/// Whether @p pointer, after `config:`, is slash-separated non-empty steps, each `*` or holding no `*`.
[[nodiscard]] bool isRootPointer(std::string_view pointer)
{
    return !pointer.empty() && std::ranges::all_of(wordsOfPath(pointer), [](std::string_view step) {
        return !step.empty() && (step == "*" || !step.contains('*'));
    });
}

/// One line split at its ` = `.
struct ManifestLine
{
    LineKind const* kind;
    std::vector<std::string_view> head;
    std::string_view value;
    RootCreation creation; ///< Whether a `root` line said `create`.
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
    if (keyword == "root" && (!line.value.starts_with("config:") || !isRootPointer(line.value.substr(7))))
        return Fault::MalformedRoot;
    if ((keyword == "command-env" || keyword == "command-env-for" || keyword == "pane-env")
        && !isEnvironmentName(head.back()))
        return Fault::MalformedEnvironmentName;
    // Only one directory can be created: a pointer that walks a list names any number of them.
    if (keyword == "root" && line.creation == RootCreation::CreateIfMissing && line.value.contains('*'))
        return Fault::CreateOverList;
    if (keyword == "command" && head[2] != "short" && head[2] != "long")
        return Fault::UnknownLine;
    if (keyword == "arg" && line.value.starts_with("path:"))
        return state.isDeclared("root", line.value.substr(5)) ? Fault::None : Fault::UndeclaredRoot;
    if (keyword == "arg" && !std::ranges::contains(PlainKinds, line.value))
        return Fault::UnknownKind;
    if ((keyword == "command-env-for" && !state.isDeclared("command", head[1]))
        || (keyword == "pane-env" && !state.isDeclared("pane", head[1])))
        return Fault::UndeclaredTarget;
    if ((keyword == "title" || keyword == "action") && !isShownAsWritten(line.value))
        return Fault::BadTitle;
    if (keyword == "command" || keyword == "pane")
        return checkArgv(line.value, state.declared);
    return Fault::None;
}

/// Checks a line's shape and records what it declares.
[[nodiscard]] Fault checkLine(std::string_view text, ManifestState& state)
{
    if (text.contains('\t'))
        return Fault::TabCharacter;
    // A line ending in " =" has an empty value.
    auto const equals = text.ends_with(" =") && !text.contains(" = ") ? text.size() - 2 : text.find(" = ");
    auto head = wordsOf(text.substr(0, equals));
    if (head.empty() || head.front().empty())
        return Fault::UnknownLine;
    // An array's iterator is not a pointer everywhere (MSVC), so the lookup yields one explicitly.
    auto const found = std::ranges::find(LineKinds, head.front(), &LineKind::keyword);
    auto const* const kind = found == LineKinds.end() ? nullptr : &*found;
    auto creation = RootCreation::Existing;
    if (kind != nullptr && kind->keyword == "root" && head.size() == kind->headWords + 1)
    {
        if (head.back() != RootCreateWord)
            return Fault::UnknownRootFlag;
        head.pop_back();
        creation = RootCreation::CreateIfMissing;
    }
    auto const line = ManifestLine {
        .kind = kind,
        .head = std::move(head),
        .value = equals == std::string_view::npos ? ""sv : text.substr(std::min(equals + 3, text.size())),
        .creation = creation,
    };
    if (line.kind == nullptr)
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
    // The kind is reserved: the grammar names it so that no version reads it as an unknown line.
    if (keyword == "inbox")
        return Fault::UnsupportedLine;
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

/// The line-grammar half of manifest version 2, independent of the tables it is checked against: a carriage
/// return right before a line feed belongs to the line end; lines of nothing but blanks and '#' lines are
/// skipped; every other line neither begins nor ends with a blank, holds no control character but a tab
/// (which checkLine() refuses on its own), is checked in order, and a name must be declared before use.
[[nodiscard]] Fault checkManifest(std::string_view text)
{
    auto state = ManifestState {};
    for (auto const rawLine: std::views::split(text, '\n'))
    {
        auto line = std::string_view(rawLine.begin(), rawLine.end());
        if (line.ends_with('\r'))
            line.remove_suffix(1);
        if (line.find_first_not_of(' ') == std::string_view::npos || line.starts_with('#'))
            continue;
        if (line.starts_with(' ') || line.ends_with(' '))
            return Fault::SurroundingBlank;
        // A tab is refused by checkLine(), which names it more precisely.
        if (std::ranges::any_of(line, [](char c) {
                auto const byte = static_cast<unsigned char>(c);
                return c != '\t' && (byte < 0x20 || byte == 0x7F);
            }))
            return Fault::ControlCharacter;
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
    CorpusRow { .file = "valid/crlf.manifest", .expected = Fault::None },
    CorpusRow { .file = "valid/orchestrator.manifest", .expected = Fault::None },
    CorpusRow { .file = "valid/pane-env.manifest", .expected = Fault::None },
    CorpusRow { .file = "valid/pane.manifest", .expected = Fault::None },
    CorpusRow { .file = "valid/placement.manifest", .expected = Fault::None },
    CorpusRow { .file = "valid/read.manifest", .expected = Fault::None },
    CorpusRow { .file = "valid/root-create.manifest", .expected = Fault::None },
    CorpusRow { .file = "valid/root.manifest", .expected = Fault::None },
    CorpusRow { .file = "valid/serve.manifest", .expected = Fault::None },
    CorpusRow { .file = "valid/title.manifest", .expected = Fault::None },
    CorpusRow { .file = "invalid/brace-inside-word.manifest", .expected = Fault::BadPlaceholder },
    CorpusRow { .file = "invalid/command-without-root.manifest", .expected = Fault::MissingRoot },
    CorpusRow { .file = "invalid/duplicate-command.manifest", .expected = Fault::Duplicate },
    CorpusRow { .file = "invalid/empty-argv.manifest", .expected = Fault::EmptyArgv },
    CorpusRow { .file = "invalid/empty-head.manifest", .expected = Fault::UnknownLine },
    CorpusRow { .file = "invalid/empty-name.manifest", .expected = Fault::EmptyValue },
    CorpusRow { .file = "invalid/environment-name.manifest", .expected = Fault::MalformedEnvironmentName },
    CorpusRow { .file = "invalid/forward-reference.manifest", .expected = Fault::UndeclaredRoot },
    CorpusRow { .file = "invalid/indented-line.manifest", .expected = Fault::SurroundingBlank },
    CorpusRow { .file = "invalid/inbox-reserved.manifest", .expected = Fault::UnsupportedLine },
    CorpusRow { .file = "invalid/env-for-undeclared-command.manifest", .expected = Fault::UndeclaredTarget },
    CorpusRow { .file = "invalid/path-of-undeclared-root.manifest", .expected = Fault::UndeclaredRoot },
    CorpusRow { .file = "invalid/placeholder-program.manifest", .expected = Fault::PlaceholderProgram },
    CorpusRow { .file = "invalid/quoted-word.manifest", .expected = Fault::QuotedWord },
    CorpusRow { .file = "invalid/read-undeclared-root.manifest", .expected = Fault::UndeclaredRoot },
    CorpusRow { .file = "invalid/root-create-over-list.manifest", .expected = Fault::CreateOverList },
    CorpusRow { .file = "invalid/root-empty-step.manifest", .expected = Fault::MalformedRoot },
    CorpusRow { .file = "invalid/root-out-of-place.manifest", .expected = Fault::MissingRoot },
    CorpusRow { .file = "invalid/root-partial-wildcard.manifest", .expected = Fault::MalformedRoot },
    CorpusRow { .file = "invalid/root-unknown-flag.manifest", .expected = Fault::UnknownRootFlag },
    CorpusRow { .file = "invalid/stray-cr.manifest", .expected = Fault::ControlCharacter },
    CorpusRow { .file = "invalid/tab-in-line.manifest", .expected = Fault::TabCharacter },
    CorpusRow { .file = "invalid/trailing-blank.manifest", .expected = Fault::SurroundingBlank },
    CorpusRow { .file = "invalid/undeclared-placeholder.manifest", .expected = Fault::UndeclaredPlaceholder },
    CorpusRow { .file = "invalid/unknown-kind.manifest", .expected = Fault::UnknownKind },
    CorpusRow { .file = "invalid/unknown-line.manifest", .expected = Fault::UnknownLine },
    CorpusRow { .file = "invalid/version-not-first.manifest", .expected = Fault::MissingHeader },
    CorpusRow { .file = "invalid/version-1-with-v2-line.manifest", .expected = Fault::VersionMismatch },
    CorpusRow { .file = "invalid/title-too-long.manifest", .expected = Fault::BadTitle },
    CorpusRow { .file = "invalid/title-control-character.manifest", .expected = Fault::ControlCharacter },
    CorpusRow { .file = "invalid/title-repeated.manifest", .expected = Fault::Duplicate },
    CorpusRow { .file = "invalid/title-bidi-override.manifest", .expected = Fault::BadTitle },
    CorpusRow { .file = "invalid/action-title-too-long.manifest", .expected = Fault::BadTitle },
};

/// One manifest written inline and the verdict it pins: the boundaries and placements the corpus files leave
/// out.
struct InlineRow
{
    std::string_view text;
    Fault expected;
};

constexpr auto InlineRows = std::array {
    InlineRow { .text = "name = agents\nversion = 2\ntitle = Agents\n", .expected = Fault::None },
    InlineRow { .text = "name = agents\ntitle = Agents\nversion = 2\n", .expected = Fault::MissingHeader },
    InlineRow { .text =
                    "name = agents\nversion = 2\ntitle = Agents \xC2\xB7 exactly forty characters, it is\n",
                .expected = Fault::None },
    InlineRow { .text =
                    "name = agents\nversion = 2\ntitle = Agents \xC2\xB7 forty-one characters, every one!\n",
                .expected = Fault::BadTitle },
    InlineRow { .text = "name = agents\nversion = 2\ntitle = Agents\x07\n",
                .expected = Fault::ControlCharacter },
    InlineRow { .text = "name = agents\nversion = 2\ntitle = Agents\ntitle = Others\n",
                .expected = Fault::Duplicate },
    InlineRow { .text = "name = agents\nversion = 1\ntitle = Agents\n", .expected = Fault::VersionMismatch },
    InlineRow { .text = "name = agents\nversion = 2\ntitle = Age\xE2\x80\x8Bnts\n",
                .expected = Fault::BadTitle },
    InlineRow { .text = "name = agents\nversion = 2\ntitle = Agents\xC2\x85\n", .expected = Fault::BadTitle },
    InlineRow { .text = "name = agents\nversion = 2\naction open = Open \xE2\x81\xA6"
                        "Agents\n",
                .expected = Fault::BadTitle },
    InlineRow {
        .text = "name = agents\nversion = 2\naction open = Open Agents \xC2\xB7 exactly forty characters!!\n",
        .expected = Fault::None },
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
    // Every cleanup command carries git-status's environment, so none can miss a variable another has.
    auto const* const status = findCommand("git-status");
    REQUIRE(status != nullptr);
    for (auto const name: CleanupCommands)
    {
        CAPTURE(name);
        auto const* const decl = findCommand(name);
        REQUIRE(decl != nullptr);
        CHECK(envValue(decl->environment, "GIT_OPTIONAL_LOCKS") == "0");
        for (auto const& variable: status->environment)
        {
            CAPTURE(variable.name);
            CHECK(envValue(decl->environment, variable.name) == variable.value);
        }
    }
}

TEST_CASE("every command keeps the forced git configuration, and only cleanup drops the attributes file",
          "[orchestrator][commands][env-for-cleanup-only]")
{
    for (auto const& decl: commands())
    {
        CAPTURE(decl.name);
        auto const environment = effectiveEnvironment(decl);
        auto const pairs = forcedConfiguration(environment);
        REQUIRE(pairs.has_value());
        auto const cleanup = std::ranges::contains(CleanupCommands, decl.name);
        auto expected = std::size_t { 0 };
        for (auto const& row: ForcedPairs)
        {
            CAPTURE(row.key);
            auto const reaches = row.reach == Reach::Every || cleanup;
            auto const forced = std::ranges::any_of(*pairs, [&](EnvironmentVariable const& pair) {
                return pair.name == row.key && pair.value == row.value;
            });
            CHECK(forced == reaches);
            if (reaches)
                ++expected;
        }
        // No other key is forced, and no stray pair lies beyond the count.
        CHECK(pairs->size() == expected);
        auto const gitConfig = std::ranges::count_if(
            environment, [](auto const& variable) { return variable.first.starts_with("GIT_CONFIG_"); });
        CHECK(static_cast<std::size_t>(gitConfig) == (2 * pairs->size()) + 1);
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
    static constexpr auto IndexFlags = std::array { "git"sv, "ls-files"sv, "-v"sv, "--stage"sv };
    static constexpr auto CommonDir =
        std::array { "git"sv, "rev-parse"sv, "--path-format=absolute"sv, "--git-common-dir"sv };
    static constexpr auto Rows = std::array {
        Row { .command = "git-common-dir", .argv = CommonDir },
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
    // A command's own environment sets GIT_CONFIG_* only as a whole set: its count replaces
    // commandEnvironment()'s, so it carries every pair it counts.
    for (auto const& decl: commands())
    {
        CAPTURE(decl.name);
        auto const own = std::ranges::count_if(
            decl.environment, [](auto const& variable) { return variable.name.starts_with("GIT_CONFIG_"); });
        CHECK((own == 0 || hasEnv(decl, "GIT_CONFIG_COUNT")));
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
        "command git-index-flags short @worktrees = git ls-files -v --stage"sv,
        "command git-common-dir short @worktrees = git rev-parse --path-format=absolute --git-common-dir"sv,
        "command-env-for git-common-dir GIT_CONFIG_COUNT = 3"sv,
        "command-env-for git-status GIT_CONFIG_KEY_2 = core.attributesFile"sv,
        "command-env-for git-worktree-remove GIT_CONFIG_VALUE_1 = /dev/null"sv,
        "command-env GIT_CONFIG_COUNT = 2"sv,
        "command-env-for git-operation-state LC_ALL = C"sv,
        "command git-worktree-remove long @worktrees = git worktree remove {path}"sv,
        "arg path = path:worktrees"sv,
        "root worktrees create = config:worktree_root"sv,
        "root repos = config:repos/*/path"sv,
    };
    auto const lines = manifestLines();
    for (auto const line: Expected)
    {
        CAPTURE(line);
        CHECK(std::ranges::contains(lines, line));
    }
}

TEST_CASE("agent-transcript runs in the worktree root", "[orchestrator][commands][transcript]")
{
    auto const* const transcript = findCommand("agent-transcript");
    REQUIRE(transcript != nullptr);
    CHECK(transcript->root == Root::Worktrees);
    CHECK(transcript->timeout == Timeout::Short);
    CHECK(transcript->environment.empty());
    CHECK(std::ranges::equal(transcript->argv,
                             std::array { "contour"sv,
                                          "agent"sv,
                                          "transcript"sv,
                                          "--session"sv,
                                          "{session}"sv,
                                          "--cwd"sv,
                                          "{path}"sv,
                                          "--page"sv,
                                          "{page}"sv,
                                          "--format"sv,
                                          "{format}"sv,
                                          "--json"sv }));
    for (auto const name: { "page"sv, "format"sv })
    {
        CAPTURE(name);
        auto const* const argument = findArgumentDecl(name);
        REQUIRE(argument != nullptr);
        CHECK(argument->kind == ArgumentKind::Number);
    }
    CHECK(std::ranges::contains(
        manifestLines(),
        "command agent-transcript short @worktrees = contour agent transcript --session "
        "{session} --cwd {path} --page {page} --format {format} --json"sv));
}

TEST_CASE("unmeasured commands are not in the manifest", "[orchestrator][commands][manifest]")
{
    auto const lines = manifestLines();
    auto unmeasured = 0;
    for (auto const& decl: commands())
    {
        CAPTURE(decl.name);
        auto const declared = std::ranges::any_of(lines, [&](std::string const& line) {
            return line.starts_with(std::format("command {} ", decl.name));
        });
        CHECK(declared == (decl.verification == Verification::Recorded));
        if (decl.verification == Verification::Unmeasured)
            ++unmeasured;
    }
    // GitLab's five, until their output is recorded.
    CHECK(unmeasured == 5);
    CHECK(std::ranges::none_of(lines, [](std::string const& line) { return line.contains("glab"); }));
    CHECK(manifestText() == readFixture(FixtureRoot::Contract, "manifest-v2/valid/orchestrator.manifest"));
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
                if (!name.ends_with(".consent.txt")) // what the consent dialog shows for a manifest, not one
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
        CHECK(manifestText()
              == readFixture(FixtureRoot::Contract, "manifest-v2/valid/orchestrator.manifest"));
    }
}

TEST_CASE("the line-grammar checker reads a title line", "[orchestrator][commands][manifest]")
{
    for (auto const& row: InlineRows)
    {
        CAPTURE(row.text, row.expected);
        CHECK(checkManifest(row.text) == row.expected);
    }
}

TEST_CASE("manifestText declares the worktree root as created on demand and posts nothing",
          "[orchestrator][commands][manifest]")
{
    auto const text = manifestText();
    CHECK(text.starts_with("name = orchestrator\nversion = 2\ntitle = Agents\nplacement daemon\n"));
    CHECK(text.ends_with('\n'));
    CHECK(text.contains("\nroot worktrees create = config:worktree_root\n"));
    CHECK(text.contains("\nroot repos = config:repos/*/path\n"));
    for (auto const line: std::views::split(text, '\n'))
    {
        auto const view = std::string_view(line.begin(), line.end());
        CAPTURE(view);
        CHECK_FALSE(view.starts_with("inbox "));
    }
}

TEST_CASE("shellQuoted quotes what a shell would split or expand", "[orchestrator][commands][shell]")
{
    struct Row
    {
        std::vector<std::string> argv;
        std::string_view expected;
    };
    auto const rows = std::array {
        Row { .argv = { "claude", "--model", "sonnet" }, .expected = "claude --model sonnet" },
        Row { .argv = { "fix https://x/pull/1" }, .expected = "'fix https://x/pull/1'" },
        Row { .argv = { "it's" }, .expected = R"('it'\''s')" },
        Row { .argv = { "", "$HOME", "a*b", "~", "#x" }, .expected = R"('' '$HOME' 'a*b' '~' '#x')" },
    };
    for (auto const& row: rows)
    {
        CAPTURE(row.expected);
        CHECK(shellQuoted(row.argv) == row.expected);
    }
}

#ifndef _WIN32
TEST_CASE("shellQuoted round-trips through sh", "[orchestrator][commands][shell]")
{
    // A word holding a ' and a space, a newline and what a shell expands: sh must print each back as one
    // word.
    auto const argv =
        std::vector<std::string> { "printf", "%s\\n", "it's a note", "$HOME `id` \"q\"", "a\nb" };
    auto const script = shellQuoted(argv);
    auto const command = shellQuoted(std::vector<std::string> { "sh", "-c", script });
    auto* const pipe = popen(command.c_str(), "r");
    REQUIRE(pipe != nullptr);
    auto output = std::string {};
    auto buffer = std::array<char, 256> {};
    while (auto const read = std::fread(buffer.data(), 1, buffer.size(), pipe))
        output.append(buffer.data(), read);
    REQUIRE(pclose(pipe) == 0);
    CHECK(output == "it's a note\n$HOME `id` \"q\"\na\nb\n");
}
#endif
