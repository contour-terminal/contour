// SPDX-License-Identifier: Apache-2.0
#include <vtextension/Manifest.hpp>

#include <core/Sha256.hpp>
#include <core/Utils.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <vtextension/SettableEnvironment.hpp>
#include <vtextension/UserSettableEnvironment.hpp>
#include <vtextension/Utf8.hpp>
#include <vtextension/wasm/WasmBinary.hpp>

namespace vtextension
{

namespace
{
    using Bytes = std::span<std::byte const>;

    /// Whether @p c may appear in a placeholder name: ASCII letters, digits, `_` and `-`.
    [[nodiscard]] constexpr bool isPlaceholderChar(char c) noexcept
    {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'
               || c == '-';
    }

    /// Whether @p name is an environment variable name: `[A-Za-z_][A-Za-z0-9_]*`.
    [[nodiscard]] constexpr bool isEnvironmentName(std::string_view name) noexcept
    {
        return !name.empty() && (name.front() < '0' || name.front() > '9')
               && std::ranges::all_of(name, [](char c) { return c != '-' && isPlaceholderChar(c); });
    }

    /// Copies @p bytes into a string.
    [[nodiscard]] std::string asText(Bytes bytes)
    {
        auto text = std::string();
        text.reserve(bytes.size());
        std::ranges::transform(
            bytes, std::back_inserter(text), [](std::byte byte) { return static_cast<char>(byte); });
        return text;
    }

    /// Whether @p codePoint is a C0 or C1 control character or DEL.
    [[nodiscard]] constexpr bool isControl(char32_t codePoint) noexcept
    {
        return codePoint < 0x20 || (codePoint >= 0x7F && codePoint <= 0x9F);
    }

    /// Whether @p text is valid UTF-8 whose only control characters are line feeds, tabs, and a
    /// carriage return immediately before a line feed.
    [[nodiscard]] bool isSoundText(std::string_view text) noexcept
    {
        while (!text.empty())
        {
            auto const decoded = decodeUtf8(text);
            if (!decoded)
                return false;
            auto const [codePoint, length] = *decoded;
            auto const isAllowed = codePoint == U'\n' || codePoint == U'\t'
                                   || (codePoint == U'\r' && text.substr(length).starts_with('\n'));
            if (isControl(codePoint) && !isAllowed)
                return false;
            text.remove_prefix(length);
        }
        return true;
    }

    /// What a scan for the manifest section found.
    enum class ScanFailure : uint8_t
    {
        Malformed, ///< The module cannot be walked to its end.
        NotFound,  ///< The module is sound and has no manifest section.
    };

    /// Walks a core module's sections for the manifest's custom section.
    /// @return The section's content (the bytes after its name); malformed if there are two.
    [[nodiscard]] std::expected<Bytes, ScanFailure> findManifestSection(Bytes module)
    {
        auto const all = wasm::binary::sections(module);
        if (!all)
            return std::unexpected(ScanFailure::Malformed);
        auto found = std::optional<Bytes> {};
        for (auto const& section: *all | std::views::filter([](wasm::binary::Section const& candidate) {
                 return candidate.id == wasm::binary::CustomSectionId;
             }))
        {
            auto reader = wasm::binary::Reader { section.content };
            auto const name = reader.name();
            if (!name)
                return std::unexpected(ScanFailure::Malformed);
            if (!std::ranges::equal(*name, ManifestSectionName, [](std::byte a, char b) {
                    return static_cast<char>(a) == b;
                }))
                continue;
            // Linkers concatenate same-named sections; a second one is a smuggling attempt, not a manifest.
            if (found)
                return std::unexpected(ScanFailure::Malformed);
            found = reader.rest();
        }
        if (!found)
            return std::unexpected(ScanFailure::NotFound);
        return *found;
    }

    [[nodiscard]] std::string_view trim(std::string_view text) noexcept
    {
        constexpr auto Blank = std::string_view(" \r");
        auto const first = text.find_first_not_of(Blank);
        if (first == std::string_view::npos)
            return {};
        return text.substr(first, text.find_last_not_of(Blank) - first + 1);
    }

    /// Splits at blanks; consecutive blanks make no empty element. Version 1's splitting.
    [[nodiscard]] std::vector<std::string_view> words(std::string_view text)
    {
        auto result = std::vector<std::string_view>();
        for (auto const word: std::views::split(text, ' ') | std::views::transform([](auto part) {
                                  return std::string_view(part.begin(), part.end());
                              }))
        {
            if (auto const trimmed = trim(word); !trimmed.empty())
                result.push_back(trimmed);
        }
        return result;
    }

    /// Splits at every single space: two blanks in a row make an empty element. Version 2's splitting.
    [[nodiscard]] std::vector<std::string_view> exactWords(std::string_view text)
    {
        auto result = std::vector<std::string_view>();
        for (auto const word: std::views::split(text, ' '))
            result.emplace_back(word.begin(), word.end());
        return result;
    }

    /// What a line does to the parse: nothing, or why the manifest is invalid.
    using LineResult = std::expected<void, ManifestFault>;

    [[nodiscard]] LineResult refuse(ManifestFault why) noexcept
    {
        return std::unexpected(why);
    }

    // {{{ the version

    /// The version line's value, if @p line is one: `version` alone before the first `=`.
    [[nodiscard]] std::optional<std::string_view> versionValue(std::string_view line)
    {
        auto const equals = line.find('=');
        if (equals == std::string_view::npos)
            return std::nullopt;
        auto const key = words(line.substr(0, equals));
        if (key.size() != 1 || key.front() != "version")
            return std::nullopt;
        return trim(line.substr(equals + 1));
    }

    /// How a line reaches the parse: trimmed, as version 1 reads it, or as written, as version 2 does.
    enum class LineBlanks : uint8_t
    {
        Trimmed, ///< Blanks around the line are dropped.
        Kept,    ///< Only the carriage return of a CRLF line end is dropped; the line is judged as written.
    };

    /// The lines that are neither blank (nothing but blanks) nor a comment, without their line end.
    [[nodiscard]] auto contentLines(std::string_view text, LineBlanks blanks)
    {
        return std::views::split(text, '\n') | std::views::transform([blanks](auto part) {
                   auto line = std::string_view(part.begin(), part.end());
                   if (blanks == LineBlanks::Trimmed)
                       return trim(line);
                   if (line.ends_with('\r'))
                       line.remove_suffix(1);
                   return line;
               })
               | std::views::filter([](std::string_view line) {
                     return line.find_first_not_of(' ') != std::string_view::npos && !line.starts_with('#');
                 });
    }

    /// Reads the one version line, wherever it stands.
    [[nodiscard]] std::expected<uint32_t, ManifestFault> readVersion(std::string_view text)
    {
        auto value = std::optional<std::string_view> {};
        for (auto const line: contentLines(text, LineBlanks::Trimmed))
        {
            auto const found = versionValue(line);
            if (!found)
                continue;
            if (value)
                return std::unexpected(ManifestFault::Repeated);
            value = found;
        }
        if (!value)
            return std::unexpected(ManifestFault::MissingVersion);
        auto parsed = uint32_t { 0 };
        auto const [end, error] = std::from_chars(value->data(), value->data() + value->size(), parsed);
        if (error != std::errc {} || end != value->data() + value->size()
            || !std::ranges::contains(SupportedManifestVersions, parsed))
            return std::unexpected(ManifestFault::UnsupportedVersion);
        return parsed;
    }

    // }}}
    // {{{ lines

    /// One line taken apart: its keyword, the words between it and the `=`, and its value.
    struct Line
    {
        std::string_view keyword;
        std::vector<std::string_view> head;    ///< The words after the keyword.
        std::optional<std::string_view> value; ///< After the `=`; std::nullopt for a line without one.
    };

    /// Takes a line apart as version 1 does: at its first `=`, blanks collapsing.
    [[nodiscard]] Line splitVersion1(std::string_view text)
    {
        auto const equals = text.find('=');
        auto head = words(text.substr(0, equals));
        auto const value =
            equals == std::string_view::npos ? std::nullopt : std::optional { trim(text.substr(equals + 1)) };
        auto const keyword = head.empty() ? std::string_view {} : head.front();
        if (!head.empty())
            head.erase(head.begin());
        return Line { .keyword = keyword, .head = std::move(head), .value = value };
    }

    /// Takes a line apart as version 2 does: at its first ` = ` (or a final ` =`), at single spaces.
    [[nodiscard]] Line splitVersion2(std::string_view text)
    {
        constexpr auto Separator = std::string_view(" = ");
        auto equals = text.find(Separator);
        auto value = std::optional<std::string_view> {};
        if (equals != std::string_view::npos)
            value = text.substr(equals + Separator.size());
        else if (text.ends_with(" ="))
        {
            equals = text.size() - 2;
            value = std::string_view {};
        }
        auto head = exactWords(text.substr(0, equals));
        auto const keyword = head.front();
        head.erase(head.begin());
        return Line { .keyword = keyword, .head = std::move(head), .value = value };
    }

    /// Whether the version line has been passed: in version 2 only `name` may come before it.
    enum class VersionLine : uint8_t
    {
        Ahead, ///< Not yet.
        Read,  ///< Passed.
    };

    /// Where the parse stands.
    struct ParseState
    {
        uint32_t version = 0;
        std::optional<std::string> name {};
        std::optional<ManifestPlacement> placement {};
        VersionLine versionLine = VersionLine::Ahead;
        Manifest manifest {};
    };

    /// Whether @p label names a root declared by an earlier line.
    [[nodiscard]] LineResult checkRoot(ParseState const& state, std::string_view label)
    {
        if (!std::ranges::contains(state.manifest.roots, label, &RootDecl::label))
            return refuse(ManifestFault::UndeclaredRoot);
        return {};
    }

    /// Whether every placeholder of a version-2 argv names an argument declared by an earlier line.
    [[nodiscard]] LineResult checkPlaceholders(ParseState const& state, std::span<std::string const> argv)
    {
        auto const isDeclared = [&](std::string const& word) {
            auto const name = placeholderName(word);
            return !name || std::ranges::contains(state.manifest.arguments, *name, &ArgumentDecl::name);
        };
        if (state.version > 1 && !std::ranges::all_of(argv, isDeclared))
            return refuse(ManifestFault::UndeclaredPlaceholder);
        return {};
    }

    /// What a line kind does with a line of its keyword.
    using LineHandler = LineResult (*)(ParseState&, Line const&);

    /// Whether a line kind takes a value, and what kind of one.
    enum class LineValue : uint8_t
    {
        None, ///< `keyword words…`.
        Text, ///< `keyword words… = value`, the value non-empty.
        Argv, ///< `keyword words… = argv…`, checked as a command line.
    };

    /// One line kind.
    struct LineKind
    {
        std::string_view keyword;
        uint32_t since;      ///< The first manifest version that knows it.
        LineValue value;     ///< What follows the `=`.
        LineHandler handler; ///< What the line does.
    };

    /// Reads an argv: at single spaces in version 2, every word sound; as today in version 1.
    [[nodiscard]] std::expected<std::vector<std::string>, ManifestFault> readArgv(uint32_t version,
                                                                                  std::string_view value)
    {
        auto const argv = version == 1 ? words(value) : exactWords(value);
        if (value.empty() || argv.empty())
            return std::unexpected(ManifestFault::EmptyArgv);
        // The program is the manifest's to name: a placeholder there would hand the guest free-form exec.
        if (placeholderName(argv.front()))
            return std::unexpected(ManifestFault::PlaceholderProgram);
        for (auto const word: argv)
        {
            if (word.empty())
                return std::unexpected(ManifestFault::UnknownLine);
            // Words are never unquoted: a quote is an attempt at a word with a space.
            if (version > 1 && (word.contains('"') || word.contains('\'')))
                return std::unexpected(ManifestFault::QuotedWord);
            if (!placeholderName(word) && (word.contains('{') || word.contains('}')))
                return std::unexpected(ManifestFault::BraceInsideWord);
        }
        return std::vector<std::string>(argv.begin(), argv.end());
    }

    /// Where the `@root` of a command or pane stands among the head words: 2 for a command (after its
    /// name and timeout class), 1 for a pane (after its name).
    [[nodiscard]] std::expected<std::string_view, ManifestFault> boundRoot(Line const& line, std::size_t at)
    {
        auto const bound =
            std::ranges::find_if(line.head, [](std::string_view word) { return word.starts_with('@'); });
        if (bound == line.head.end())
            return std::unexpected(ManifestFault::CommandWithoutRoot);
        if (std::cmp_not_equal(bound - line.head.begin(), at))
            return std::unexpected(ManifestFault::RootOutOfPlace);
        if (line.head.size() != at + 1)
            return std::unexpected(ManifestFault::UnknownLine);
        return bound->substr(1);
    }

    [[nodiscard]] LineResult setName(ParseState& state, Line const& line)
    {
        if (!line.head.empty())
            return refuse(ManifestFault::UnknownLine);
        if (state.name)
            return refuse(ManifestFault::Repeated);
        state.name = std::string(*line.value);
        return {};
    }

    /// The timeout classes a version-2 command names.
    struct TimeoutName
    {
        std::string_view word;
        CommandTimeout timeout;
    };

    constexpr auto TimeoutNames = std::array {
        TimeoutName { .word = "short", .timeout = CommandTimeout::Short },
        TimeoutName { .word = "long", .timeout = CommandTimeout::Long },
    };

    [[nodiscard]] LineResult addCommand(ParseState& state, Line const& line)
    {
        auto command = DeclaredCommand {};
        if (state.version == 1)
        {
            // `command <name> = argv…`; a head of more words is version 2's `<name> <timeout> @<root>`.
            if (line.head.size() != 1)
                return refuse(line.head.empty() ? ManifestFault::UnknownLine
                                                : ManifestFault::Version2LineInVersion1);
        }
        else
        {
            auto const root = boundRoot(line, 2);
            if (!root)
                return std::unexpected(root.error());
            auto const timeout = std::ranges::find(TimeoutNames, line.head[1], &TimeoutName::word);
            if (timeout == TimeoutNames.end())
                return refuse(ManifestFault::UnknownTimeout);
            if (auto const declared = checkRoot(state, *root); !declared)
                return declared;
            command.timeout = timeout->timeout;
            command.root = std::string(*root);
        }
        auto argv = readArgv(state.version, *line.value);
        if (!argv)
            return std::unexpected(argv.error());
        if (auto const declared = checkPlaceholders(state, *argv); !declared)
            return declared;
        if (std::ranges::contains(state.manifest.commands, line.head.front(), &DeclaredCommand::name))
            return refuse(ManifestFault::Duplicate);
        command.name = std::string(line.head.front());
        command.argvTemplate = *std::move(argv);
        state.manifest.commands.push_back(std::move(command));
        return {};
    }

    [[nodiscard]] LineResult addControl(ParseState& state, Line const& line)
    {
        if (line.head.size() != 1)
            return refuse(ManifestFault::UnknownLine);
        auto const permission = vtcontrol::enumFromName<vtcontrol::Permission>(line.head.front());
        if (!permission || std::ranges::contains(state.manifest.control, *permission))
            return refuse(ManifestFault::UnknownPermission);
        state.manifest.control.push_back(*permission);
        return {};
    }

    /// The places a version-2 manifest names.
    struct PlacementName
    {
        std::string_view word;
        ManifestPlacement placement;
    };

    constexpr auto PlacementNames = std::array {
        PlacementName { .word = "status-line", .placement = ManifestPlacement::StatusLine },
        PlacementName { .word = "daemon", .placement = ManifestPlacement::Daemon },
    };

    [[nodiscard]] LineResult setPlacement(ParseState& state, Line const& line)
    {
        if (line.head.size() != 1)
            return refuse(ManifestFault::UnknownLine);
        auto const known = std::ranges::find(PlacementNames, line.head.front(), &PlacementName::word);
        if (known == PlacementNames.end())
            return refuse(ManifestFault::UnknownLine);
        if (state.placement)
            return refuse(ManifestFault::Repeated);
        state.placement = known->placement;
        return {};
    }

    /// The one word a root may carry between its label and its `=`.
    constexpr auto RootCreateWord = std::string_view("create");

    /// What a root's value starts with: the pointer that follows is into the extension's configuration.
    constexpr auto RootPointerPrefix = std::string_view("config:");

    /// The pointer step that walks every element of a list.
    constexpr auto RootListStep = std::string_view("*");

    [[nodiscard]] LineResult addRoot(ParseState& state, Line const& line)
    {
        if (line.head.empty() || line.head.size() > 2)
            return refuse(ManifestFault::UnknownLine);
        if (line.head.size() == 2 && line.head.back() != RootCreateWord)
            return refuse(ManifestFault::UnknownRootFlag);
        auto const creation = line.head.size() == 2 ? RootCreation::CreateIfMissing : RootCreation::Existing;
        if (!line.value->starts_with(RootPointerPrefix))
            return refuse(ManifestFault::MalformedRoot);
        auto const pointer = line.value->substr(RootPointerPrefix.size());
        auto steps = std::views::split(pointer, '/') | std::views::transform([](auto step) {
                         return std::string_view(step.begin(), step.end());
                     });
        if (pointer.empty() || !std::ranges::all_of(steps, [](std::string_view step) {
                return !step.empty() && (step == RootListStep || !step.contains('*'));
            }))
            return refuse(ManifestFault::MalformedRoot);
        // Only one directory can be created: a pointer that walks a list names any number of them.
        if (creation == RootCreation::CreateIfMissing && std::ranges::contains(steps, RootListStep))
            return refuse(ManifestFault::CreateOverList);
        if (std::ranges::contains(state.manifest.roots, line.head.front(), &RootDecl::label))
            return refuse(ManifestFault::Duplicate);
        state.manifest.roots.push_back(RootDecl {
            .label = std::string(line.head.front()), .pointer = std::string(pointer), .creation = creation });
        return {};
    }

    [[nodiscard]] LineResult addReadable(ParseState& state, Line const& line)
    {
        if (line.head.size() != 1)
            return refuse(ManifestFault::UnknownLine);
        if (auto const declared = checkRoot(state, line.head.front()); !declared)
            return declared;
        if (std::ranges::contains(state.manifest.readableRoots, line.head.front()))
            return refuse(ManifestFault::Duplicate);
        state.manifest.readableRoots.emplace_back(line.head.front());
        return {};
    }

    [[nodiscard]] LineResult addArgument(ParseState& state, Line const& line)
    {
        if (line.head.size() != 1)
            return refuse(ManifestFault::UnknownLine);
        auto const kind = argumentKindOf(*line.value);
        if (!kind)
            return refuse(ManifestFault::UnknownKind);
        auto root = *kind == ArgumentKind::Path ? std::string(line.value->substr(PathKindPrefix.size()))
                                                : std::string {};
        if (*kind == ArgumentKind::Path)
            if (auto const declared = checkRoot(state, root); !declared)
                return declared;
        if (std::ranges::contains(state.manifest.arguments, line.head.front(), &ArgumentDecl::name))
            return refuse(ManifestFault::Duplicate);
        state.manifest.arguments.push_back(
            ArgumentDecl { .name = std::string(line.head.front()), .kind = *kind, .root = std::move(root) });
        return {};
    }

    /// Reads `<KEY> = value` at the end of an environment line's head.
    [[nodiscard]] std::expected<EnvironmentEntry, ManifestFault> environmentEntry(Line const& line)
    {
        if (!isEnvironmentName(line.head.back()))
            return std::unexpected(ManifestFault::MalformedEnvironmentName);
        return EnvironmentEntry { .name = std::string(line.head.back()), .value = std::string(*line.value) };
    }

    [[nodiscard]] LineResult addCommandEnvironment(ParseState& state, Line const& line)
    {
        if (line.head.size() != 1)
            return refuse(ManifestFault::UnknownLine);
        auto entry = environmentEntry(line);
        if (!entry)
            return std::unexpected(entry.error());
        if (std::ranges::contains(state.manifest.commandEnvironment, entry->name, &EnvironmentEntry::name))
            return refuse(ManifestFault::Duplicate);
        state.manifest.commandEnvironment.push_back(*std::move(entry));
        return {};
    }

    /// Adds a `command-env-for` or `pane-env` line to the environment of the command or pane it names, which
    /// an earlier line declared.
    template <typename Target>
    [[nodiscard]] LineResult addTargetEnvironment(std::vector<Target>& targets, Line const& line)
    {
        if (line.head.size() != 2)
            return refuse(ManifestFault::UnknownLine);
        auto entry = environmentEntry(line);
        if (!entry)
            return std::unexpected(entry.error());
        auto const target = std::ranges::find(targets, line.head.front(), &Target::name);
        if (target == targets.end())
            return refuse(ManifestFault::EnvForUndeclared);
        if (std::ranges::contains(target->environment, entry->name, &EnvironmentEntry::name))
            return refuse(ManifestFault::Duplicate);
        target->environment.push_back(*std::move(entry));
        return {};
    }

    [[nodiscard]] LineResult addCommandEnvironmentFor(ParseState& state, Line const& line)
    {
        return addTargetEnvironment(state.manifest.commands, line);
    }

    [[nodiscard]] LineResult addPaneEnvironment(ParseState& state, Line const& line)
    {
        return addTargetEnvironment(state.manifest.panes, line);
    }

    /// Binds a `command-env-set` or `pane-env-set` line's command or pane, which an earlier line declared, to
    /// the user's set its `{argument}` names: an earlier `arg` of kind name. Each listed variable is an
    /// environment name, listed once, and on UserSettableVariables.
    template <typename Target>
    [[nodiscard]] LineResult bindEnvironmentSet(std::vector<ArgumentDecl> const& arguments,
                                                std::vector<Target>& targets,
                                                Line const& line)
    {
        if (line.head.size() != 2)
            return refuse(ManifestFault::UnknownLine);
        auto const target = std::ranges::find(targets, line.head.front(), &Target::name);
        if (target == targets.end())
            return refuse(ManifestFault::EnvForUndeclared);
        auto const argumentName = placeholderName(line.head.back());
        if (!argumentName)
            return refuse(ManifestFault::UnknownLine);
        auto const argument = std::ranges::find(arguments, *argumentName, &ArgumentDecl::name);
        if (argument == arguments.end())
            return refuse(ManifestFault::UndeclaredPlaceholder);
        if (argument->kind != ArgumentKind::Name)
            return refuse(ManifestFault::EnvSetNotName);
        if (target->environmentSet)
            return refuse(ManifestFault::Duplicate);
        auto binding = EnvironmentSetBinding { .argument = argument->name, .variables = {} };
        for (auto const variable: exactWords(*line.value))
        {
            if (!isEnvironmentName(variable))
                return refuse(ManifestFault::MalformedEnvironmentName);
            if (std::ranges::contains(binding.variables, variable))
                return refuse(ManifestFault::Duplicate);
            if (userSettableRowOf(variable) == nullptr)
                return refuse(ManifestFault::ForbiddenEnvironment);
            binding.variables.emplace_back(variable);
        }
        target->environmentSet = std::move(binding);
        return {};
    }

    [[nodiscard]] LineResult bindCommandEnvironmentSet(ParseState& state, Line const& line)
    {
        return bindEnvironmentSet(state.manifest.arguments, state.manifest.commands, line);
    }

    [[nodiscard]] LineResult bindPaneEnvironmentSet(ParseState& state, Line const& line)
    {
        return bindEnvironmentSet(state.manifest.arguments, state.manifest.panes, line);
    }

    [[nodiscard]] LineResult addPane(ParseState& state, Line const& line)
    {
        auto const root = boundRoot(line, 1);
        if (!root)
            return std::unexpected(root.error());
        if (auto const declared = checkRoot(state, *root); !declared)
            return declared;
        auto argv = readArgv(state.version, *line.value);
        if (!argv)
            return std::unexpected(argv.error());
        if (auto const declared = checkPlaceholders(state, *argv); !declared)
            return declared;
        if (std::ranges::contains(state.manifest.panes, line.head.front(), &PaneTemplate::name))
            return refuse(ManifestFault::Duplicate);
        state.manifest.panes.push_back(PaneTemplate { .name = std::string(line.head.front()),
                                                      .root = std::string(*root),
                                                      .argvTemplate = *std::move(argv),
                                                      .environment = {},
                                                      .environmentSet = {} });
        return {};
    }

    /// The `inbox` line kind is reserved: named, so that it is never mistaken for an unknown line, and
    /// refused until a version of the manifest supports it.
    [[nodiscard]] LineResult refuseReserved(ParseState& /*state*/, Line const& /*line*/)
    {
        return refuse(ManifestFault::UnsupportedLine);
    }

    /// `pane-input`: no words. It lets `pane-send` type and `pane-key` press keys into the panes, so it means
    /// nothing without one.
    [[nodiscard]] LineResult allowPaneInput(ParseState& state, Line const& line)
    {
        if (!line.head.empty())
            return refuse(ManifestFault::UnknownLine);
        if (state.manifest.paneInput == PaneInput::Typed)
            return refuse(ManifestFault::Repeated);
        state.manifest.paneInput = PaneInput::Typed;
        return {};
    }

    /// `url-open`: no words. It lets `url-open` ask the frontend the user is at to open a web link.
    [[nodiscard]] LineResult allowUrlOpen(ParseState& state, Line const& line)
    {
        if (!line.head.empty())
            return refuse(ManifestFault::UnknownLine);
        if (state.manifest.urlOpening == UrlOpening::Allowed)
            return refuse(ManifestFault::Repeated);
        state.manifest.urlOpening = UrlOpening::Allowed;
        return {};
    }

    [[nodiscard]] LineResult setServedScheme(ParseState& state, Line const& line)
    {
        if (line.head.size() != 1)
            return refuse(ManifestFault::UnknownLine);
        if (state.manifest.servedScheme)
            return refuse(ManifestFault::Repeated);
        state.manifest.servedScheme = std::string(line.head.front());
        return {};
    }

    /// Whether @p title is shown as written: at most MaxTitleLength code points, none that disguises text
    /// (DisguisingCodePoints). isSoundText() has already found the text to be UTF-8.
    [[nodiscard]] bool isShownAsWritten(std::string_view title) noexcept
    {
        auto count = std::size_t { 0 };
        while (!title.empty())
        {
            auto const decoded = decodeUtf8(title);
            if (!decoded || disguisesText(decoded->first) || ++count > MaxTitleLength)
                return false;
            title.remove_prefix(decoded->second);
        }
        return true;
    }

    [[nodiscard]] LineResult addAction(ParseState& state, Line const& line)
    {
        if (line.head.size() != 1)
            return refuse(ManifestFault::UnknownLine);
        if (std::ranges::contains(state.manifest.actions, line.head.front(), &ActionDecl::id))
            return refuse(ManifestFault::Duplicate);
        if (!isShownAsWritten(*line.value))
            return refuse(ManifestFault::BadTitle);
        state.manifest.actions.push_back(
            ActionDecl { .id = std::string(line.head.front()), .title = std::string(*line.value) });
        return {};
    }

    /// A title is shown as written: a control character is refused anywhere in the text before a line is
    /// read, and what else would disguise it is refused here, as is a title longer than MaxTitleLength.
    [[nodiscard]] LineResult setTitle(ParseState& state, Line const& line)
    {
        if (!line.head.empty())
            return refuse(ManifestFault::UnknownLine);
        if (!state.manifest.title.empty())
            return refuse(ManifestFault::Repeated);
        if (!isShownAsWritten(*line.value))
            return refuse(ManifestFault::BadTitle);
        state.manifest.title = std::string(*line.value);
        return {};
    }

    /// Every line kind but `version`, which is read before them all (readVersion()).
    constexpr auto LineKinds = std::array {
        LineKind { .keyword = "name", .since = 1, .value = LineValue::Text, .handler = &setName },
        LineKind { .keyword = "command", .since = 1, .value = LineValue::Argv, .handler = &addCommand },
        LineKind { .keyword = "control", .since = 1, .value = LineValue::None, .handler = &addControl },
        LineKind { .keyword = "placement", .since = 2, .value = LineValue::None, .handler = &setPlacement },
        LineKind { .keyword = "root", .since = 2, .value = LineValue::Text, .handler = &addRoot },
        LineKind { .keyword = "read", .since = 2, .value = LineValue::None, .handler = &addReadable },
        LineKind { .keyword = "arg", .since = 2, .value = LineValue::Text, .handler = &addArgument },
        LineKind { .keyword = "command-env",
                   .since = 2,
                   .value = LineValue::Text,
                   .handler = &addCommandEnvironment },
        LineKind { .keyword = "command-env-for",
                   .since = 2,
                   .value = LineValue::Text,
                   .handler = &addCommandEnvironmentFor },
        LineKind { .keyword = "command-env-set",
                   .since = 2,
                   .value = LineValue::Text,
                   .handler = &bindCommandEnvironmentSet },
        LineKind { .keyword = "pane", .since = 2, .value = LineValue::Argv, .handler = &addPane },
        LineKind { .keyword = "pane-env-set",
                   .since = 2,
                   .value = LineValue::Text,
                   .handler = &bindPaneEnvironmentSet },
        LineKind {
            .keyword = "pane-env", .since = 2, .value = LineValue::Text, .handler = &addPaneEnvironment },
        LineKind { .keyword = "inbox", .since = 2, .value = LineValue::Text, .handler = &refuseReserved },
        LineKind { .keyword = "serve", .since = 2, .value = LineValue::None, .handler = &setServedScheme },
        LineKind {
            .keyword = "pane-input", .since = 2, .value = LineValue::None, .handler = &allowPaneInput },
        LineKind { .keyword = "url-open", .since = 2, .value = LineValue::None, .handler = &allowUrlOpen },
        LineKind { .keyword = "action", .since = 2, .value = LineValue::Text, .handler = &addAction },
        LineKind { .keyword = "title", .since = 2, .value = LineValue::Text, .handler = &setTitle },
    };

    /// Judges one line under the manifest's version.
    [[nodiscard]] LineResult applyLine(ParseState& state, std::string_view text)
    {
        if (text.starts_with('='))
            return refuse(ManifestFault::MissingKeyword);
        auto const line = state.version == 1 ? splitVersion1(text) : splitVersion2(text);
        auto const kind = std::ranges::find(LineKinds, line.keyword, &LineKind::keyword);
        if (kind == LineKinds.end())
            return refuse(ManifestFault::UnknownLine);
        if (kind->since > state.version)
            return refuse(ManifestFault::Version2LineInVersion1);
        if (state.version > 1 && state.versionLine == VersionLine::Ahead && kind->keyword != "name")
            return refuse(ManifestFault::VersionNotFirst);
        if (line.value.has_value() != (kind->value != LineValue::None)
            || std::ranges::contains(line.head, std::string_view {}))
            return refuse(ManifestFault::UnknownLine);
        // An empty argv is refused by the argv check, which names the fault more precisely.
        if (kind->value == LineValue::Text && line.value->empty())
            return refuse(ManifestFault::EmptyName);
        return kind->handler(state, line);
    }

    // }}}
} // namespace

std::string_view manifestFaultText(ManifestFault fault) noexcept
{
    return ManifestFaultTexts.at(std::to_underlying(fault)).text;
}

std::string_view displayTitleOf(Manifest const& manifest) noexcept
{
    return manifest.title.empty() ? std::string_view(manifest.name) : std::string_view(manifest.title);
}

std::optional<std::string_view> placeholderName(std::string_view element) noexcept
{
    if (element.size() < 3 || !element.starts_with('{') || !element.ends_with('}'))
        return std::nullopt;
    auto const name = element.substr(1, element.size() - 2);
    if (!std::ranges::all_of(name, isPlaceholderChar))
        return std::nullopt;
    return name;
}

std::expected<Manifest, ManifestFault> parseManifestText(std::string_view text)
{
    if (!isSoundText(text))
        return std::unexpected(ManifestFault::NotUtf8);
    // Version 1 reads a tab as a blank; make it the one kind the splitting knows. Version 2 refuses tabs.
    auto blanked = std::string(text);
    std::ranges::replace(blanked, '\t', ' ');

    auto const version = readVersion(blanked);
    if (!version)
        return std::unexpected(version.error());
    auto state = ParseState { .version = *version };
    // Version 2 takes every line as written: words are separated by single spaces, and a value is never
    // trimmed.
    auto lines =
        *version == 1 ? contentLines(blanked, LineBlanks::Trimmed) : contentLines(text, LineBlanks::Kept);
    for (auto const line: lines)
    {
        if (line.contains('\t'))
            return std::unexpected(ManifestFault::TabCharacter);
        if (line.starts_with(' ') || line.ends_with(' '))
            return std::unexpected(ManifestFault::SurroundingBlank);
        if (versionValue(line))
        {
            state.versionLine = VersionLine::Read;
            continue;
        }
        if (auto const applied = applyLine(state, line); !applied)
            return std::unexpected(applied.error());
    }
    if (!state.name)
        return std::unexpected(ManifestFault::MissingName);
    auto manifest = std::move(state.manifest);
    manifest.name = *std::move(state.name);
    manifest.version = state.version;
    manifest.placement = state.placement.value_or(ManifestPlacement::StatusLine);
    // An environment reaches a program the user granted: one that makes it run another is refused.
    if (auto const settable = checkSettableEnvironments(manifest); !settable)
        return std::unexpected(settable.error());
    return manifest;
}

std::expected<Manifest, ExtensionError> parseManifest(std::span<std::byte const> module)
{
    auto const section = findManifestSection(module);
    if (!section)
        return std::unexpected(section.error() == ScanFailure::NotFound ? ExtensionError::ManifestMissing
                                                                        : ExtensionError::ManifestInvalid);
    return parseManifestText(asText(*section))
        .transform([&](Manifest manifest) {
            // The whole module, not its manifest section: a grant covers the code it was given for, and a
            // module whose code changes is asked about anew, whatever its manifest says.
            manifest.digest = core::toLowerHexString(core::Sha256::hash(module));
            return manifest;
        })
        .transform_error([](ManifestFault) { return ExtensionError::ManifestInvalid; });
}

bool hasStartFunction(std::span<std::byte const> module) noexcept
{
    auto const all = wasm::binary::sections(module);
    return all && std::ranges::contains(*all, wasm::binary::StartSectionId, &wasm::binary::Section::id);
}

} // namespace vtextension
