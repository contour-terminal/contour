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

    /// Splits at blanks; consecutive blanks make no empty element.
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

    /// Whether @p element is sound as an argv template element: braces appear only as a whole
    /// `{name}` element.
    [[nodiscard]] bool isSoundElement(std::string_view element) noexcept
    {
        return placeholderName(element).has_value() || (!element.contains('{') && !element.contains('}'));
    }

    /// One line of the manifest, split into its key and its value.
    struct Line
    {
        std::vector<std::string_view> key;
        std::string_view value;
    };

    [[nodiscard]] std::optional<Line> splitLine(std::string_view line)
    {
        auto const equals = line.find('=');
        if (equals == std::string_view::npos)
            return std::nullopt;
        return Line { .key = words(line.substr(0, equals)), .value = trim(line.substr(equals + 1)) };
    }

    /// Where the parse stands: what has been read so far.
    struct ParseState
    {
        std::optional<std::string> name;
        std::optional<uint32_t> version;
        std::vector<DeclaredCommand> commands;
        std::vector<vtcontrol::Permission> control;
    };

    /// What a line does to the parse: nothing, or the reason the manifest is invalid.
    using LineResult = std::expected<void, ExtensionError>;

    /// What a scalar key does with its value.
    using ScalarHandler = LineResult (*)(ParseState&, std::string_view);

    [[nodiscard]] LineResult invalid() noexcept
    {
        return std::unexpected(ExtensionError::ManifestInvalid);
    }

    /// The manifest format revisions this host reads.
    constexpr auto SupportedVersions = std::array<uint32_t, 1> { 1 };

    [[nodiscard]] LineResult setName(ParseState& state, std::string_view value)
    {
        if (state.name || value.empty())
            return invalid();
        state.name = std::string(value);
        return {};
    }

    [[nodiscard]] LineResult setVersion(ParseState& state, std::string_view value)
    {
        if (state.version)
            return invalid();
        auto parsed = uint32_t { 0 };
        auto const [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (error != std::errc {} || end != value.data() + value.size()
            || std::ranges::find(SupportedVersions, parsed) == SupportedVersions.end())
            return invalid();
        state.version = parsed;
        return {};
    }

    /// The keys of the form `<key> = <value>`.
    struct ScalarKey
    {
        std::string_view key;
        ScalarHandler handler;
    };

    constexpr auto ScalarKeys = std::array {
        ScalarKey { .key = "name", .handler = &setName },
        ScalarKey { .key = "version", .handler = &setVersion },
    };

    /// The keyword of the line `command <name> = <argv...>`.
    constexpr auto CommandKeyword = std::string_view("command");

    [[nodiscard]] LineResult addCommand(ParseState& state, std::string_view name, std::string_view value)
    {
        auto argv = words(value);
        // The program is the manifest's to name: a placeholder there would hand the guest free-form exec.
        if (argv.empty() || placeholderName(argv.front()) || !std::ranges::all_of(argv, isSoundElement))
            return invalid();
        if (std::ranges::any_of(state.commands,
                                [&](DeclaredCommand const& declared) { return declared.name == name; }))
            return invalid();
        state.commands.push_back(DeclaredCommand {
            .name = std::string(name), .argvTemplate = std::vector<std::string>(argv.begin(), argv.end()) });
        return {};
    }

    /// The keyword of the line `control <permission>`, the one kind of line without a value.
    constexpr auto ControlKeyword = std::string_view("control");

    [[nodiscard]] LineResult addControl(ParseState& state, std::string_view name)
    {
        auto const permission = vtcontrol::enumFromName<vtcontrol::Permission>(name);
        if (!permission || std::ranges::contains(state.control, *permission))
            return invalid();
        state.control.push_back(*permission);
        return {};
    }

    /// Applies a line without `=`: only `control <permission>` is one.
    [[nodiscard]] LineResult applyBareLine(ParseState& state, std::string_view line)
    {
        auto const parts = words(line);
        if (parts.size() == 2 && parts.front() == ControlKeyword)
            return addControl(state, parts.back());
        return invalid();
    }

    [[nodiscard]] LineResult applyLine(ParseState& state, Line const& line)
    {
        if (line.key.size() == 1)
        {
            auto const known = std::ranges::find(ScalarKeys, line.key.front(), &ScalarKey::key);
            return known != ScalarKeys.end() ? known->handler(state, line.value) : invalid();
        }
        if (line.key.size() == 2 && line.key.front() == CommandKeyword)
            return addCommand(state, line.key.back(), line.value);
        return invalid();
    }

    /// Parses the manifest text.
    [[nodiscard]] std::expected<Manifest, ExtensionError> parseText(std::string text)
    {
        if (!isSoundText(text))
            return std::unexpected(ExtensionError::ManifestInvalid);
        // Tabs are blanks; make them the one kind the splitting knows.
        std::ranges::replace(text, '\t', ' ');

        auto state = ParseState {};
        for (auto const rawLine: std::views::split(std::string_view(text), '\n'))
        {
            auto const line = trim(std::string_view(rawLine.begin(), rawLine.end()));
            if (line.empty() || line.starts_with('#'))
                continue;
            auto const parsed = splitLine(line);
            if (auto const applied = parsed ? applyLine(state, *parsed) : applyBareLine(state, line);
                !applied)
                return std::unexpected(applied.error());
        }
        if (!state.name || !state.version)
            return std::unexpected(ExtensionError::ManifestInvalid);
        return Manifest { .name = *std::move(state.name),
                          .version = *state.version,
                          .commands = std::move(state.commands),
                          .control = std::move(state.control),
                          .digest = {} };
    }

} // namespace

std::optional<std::string_view> placeholderName(std::string_view element) noexcept
{
    if (element.size() < 3 || !element.starts_with('{') || !element.ends_with('}'))
        return std::nullopt;
    auto const name = element.substr(1, element.size() - 2);
    if (!std::ranges::all_of(name, isPlaceholderChar))
        return std::nullopt;
    return name;
}

std::expected<Manifest, ExtensionError> parseManifest(std::span<std::byte const> module)
{
    auto const section = findManifestSection(module);
    if (!section)
        return std::unexpected(section.error() == ScanFailure::NotFound ? ExtensionError::ManifestMissing
                                                                        : ExtensionError::ManifestInvalid);
    return parseText(asText(*section)).transform([&](Manifest manifest) {
        manifest.digest = core::toLowerHexString(core::Sha256::hash(*section));
        return manifest;
    });
}

bool hasStartFunction(std::span<std::byte const> module) noexcept
{
    auto const all = wasm::binary::sections(module);
    return all && std::ranges::contains(*all, wasm::binary::StartSectionId, &wasm::binary::Section::id);
}

} // namespace vtextension
