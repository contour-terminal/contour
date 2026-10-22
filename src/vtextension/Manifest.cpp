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

    /// The first eight bytes of every core module: `\0asm`, then version 1 as a little-endian u32.
    constexpr auto ModuleHeader = std::array<uint8_t, 8> { 0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00 };

    /// The id of a custom section, the only kind that carries a name.
    constexpr uint8_t CustomSectionId = 0;

    /// The most bytes a LEB128-encoded u32 takes.
    constexpr std::size_t MaxLeb128U32Bytes = 5;

    /// Of the fifth byte of a LEB128-encoded u32, only the low four bits may be set.
    constexpr uint8_t LastLeb128ByteMask = 0b0000'1111;

    /// The bits of a LEB128 byte that carry the value; the top bit says another byte follows.
    constexpr uint8_t Leb128PayloadMask = 0b0111'1111;
    constexpr uint8_t Leb128ContinueBit = 0b1000'0000;

    /// Reads bytes from a span and fails, rather than reads past it, when they run out.
    class ByteReader
    {
      public:
        explicit ByteReader(Bytes bytes) noexcept: _bytes { bytes } {}

        [[nodiscard]] bool atEnd() const noexcept { return _bytes.empty(); }

        /// Takes the next @p count bytes.
        [[nodiscard]] std::optional<Bytes> take(std::size_t count) noexcept
        {
            if (count > _bytes.size())
                return std::nullopt;
            auto const taken = _bytes.first(count);
            _bytes = _bytes.subspan(count);
            return taken;
        }

        /// Takes one byte.
        [[nodiscard]] std::optional<uint8_t> byte() noexcept
        {
            return take(1).transform([](Bytes taken) { return std::to_integer<uint8_t>(taken.front()); });
        }

        /// Takes an unsigned LEB128 integer of at most 32 bits.
        [[nodiscard]] std::optional<uint32_t> u32() noexcept
        {
            auto value = uint32_t { 0 };
            for (auto const index: std::views::iota(std::size_t { 0 }, MaxLeb128U32Bytes))
            {
                auto const next = byte();
                if (!next)
                    return std::nullopt;
                auto const isLast = index + 1 == MaxLeb128U32Bytes;
                if (isLast && (*next & static_cast<uint8_t>(~LastLeb128ByteMask)) != 0)
                    return std::nullopt;
                value |= static_cast<uint32_t>(*next & Leb128PayloadMask) << (index * 7);
                if ((*next & Leb128ContinueBit) == 0)
                    return value;
            }
            return std::nullopt;
        }

        /// Takes everything that is left.
        [[nodiscard]] Bytes rest() noexcept { return std::exchange(_bytes, Bytes {}); }

      private:
        Bytes _bytes;
    };

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

    /// The id of the start section, which names a function the runtime calls on instantiation.
    constexpr uint8_t StartSectionId = 8;

    /// What a section visitor tells the walk.
    enum class Walk : uint8_t
    {
        Continue, ///< Visit the next section.
        Fail,     ///< The module is malformed; stop.
    };

    /// Walks a core module's sections, calling @p visit with each one's id and payload.
    /// @return Whether the module could be walked to its end without @p visit failing it.
    template <typename Visit>
    [[nodiscard]] bool walkSections(Bytes module, Visit const& visit)
    {
        auto reader = ByteReader(module);
        auto const header = reader.take(ModuleHeader.size());
        if (!header || !std::ranges::equal(*header, ModuleHeader, [](std::byte a, uint8_t b) {
                return std::to_integer<uint8_t>(a) == b;
            }))
            return false;

        while (!reader.atEnd())
        {
            auto const id = reader.byte();
            auto const size = reader.u32();
            if (!id || !size)
                return false;
            auto const payload = reader.take(*size);
            if (!payload || visit(*id, *payload) == Walk::Fail)
                return false;
        }
        return true;
    }

    /// Walks a core module's sections for the manifest's custom section.
    /// @return The section's content (the bytes after its name); malformed if there are two.
    [[nodiscard]] std::expected<Bytes, ScanFailure> findManifestSection(Bytes module)
    {
        auto found = std::optional<Bytes> {};
        auto const walked = walkSections(module, [&](uint8_t id, Bytes payload) {
            if (id != CustomSectionId)
                return Walk::Continue;
            auto section = ByteReader(payload);
            auto const nameLength = section.u32();
            auto const name = nameLength ? section.take(*nameLength) : std::nullopt;
            if (!name)
                return Walk::Fail;
            if (!std::ranges::equal(*name, ManifestSectionName, [](std::byte a, char b) {
                    return static_cast<char>(a) == b;
                }))
                return Walk::Continue;
            // Linkers concatenate same-named sections; a second one is a smuggling attempt, not a manifest.
            if (found)
                return Walk::Fail;
            found = section.rest();
            return Walk::Continue;
        });
        if (!walked)
            return std::unexpected(ScanFailure::Malformed);
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
            if (!parsed)
                return std::unexpected(ExtensionError::ManifestInvalid);
            if (auto const applied = applyLine(state, *parsed); !applied)
                return std::unexpected(applied.error());
        }
        if (!state.name || !state.version)
            return std::unexpected(ExtensionError::ManifestInvalid);
        return Manifest { .name = *std::move(state.name),
                          .version = *state.version,
                          .commands = std::move(state.commands),
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
    auto found = false;
    auto const walked = walkSections(module, [&](uint8_t id, Bytes /*payload*/) {
        found = found || id == StartSectionId;
        return Walk::Continue;
    });
    return walked && found;
}

} // namespace vtextension
