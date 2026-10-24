// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/vt/ProgramStatus.hpp>

#include <core/Base64.hpp>
#include <core/Utils.hpp>

#include <algorithm>
#include <ranges>

using std::string;
using std::string_view;

namespace vtbackend
{

namespace
{
    /// The longest `id`, its longest segment, and the most segments it may have.
    constexpr auto MaxIdLength = std::size_t { 128 };
    constexpr auto MaxIdSegmentLength = std::size_t { 32 };
    constexpr auto MaxIdLevels = std::size_t { 8 };

    /// The longest `app`.
    constexpr auto MaxAppLength = std::size_t { 32 };

    /// The highest `progress`.
    constexpr auto MaxProgress = 100U;

    /// The value of the one key a report must carry to remove records rather than set one.
    constexpr auto ClearState = string_view { "clear" };

    /// Whether @p ch may appear in a value: `[A-Za-z0-9_.,+/=-]`.
    [[nodiscard]] constexpr bool isValueByte(char ch) noexcept
    {
        return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')
               || string_view { "_.,+/=-" }.contains(ch);
    }

    /// Whether @p ch may appear in an id segment or an app: `[A-Za-z0-9_.+-]`.
    [[nodiscard]] constexpr bool isNameByte(char ch) noexcept
    {
        return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')
               || string_view { "_.+-" }.contains(ch);
    }

    /// Whether @p text is `[A-Za-z0-9_.+-]{1,max}`.
    [[nodiscard]] constexpr bool isName(string_view text, std::size_t max) noexcept
    {
        return !text.empty() && text.size() <= max && std::ranges::all_of(text, isNameByte);
    }

    /// @p text without its leading and trailing blanks.
    [[nodiscard]] constexpr string_view trimmed(string_view text) noexcept
    {
        auto const isBlank = [](char ch) {
            return ch == ' ' || ch == '\t';
        };
        while (!text.empty() && isBlank(text.front()))
            text.remove_prefix(1);
        while (!text.empty() && isBlank(text.back()))
            text.remove_suffix(1);
        return text;
    }

    /// The last value each key the decoder reads was given; absent when the report did not give it.
    struct RawFields
    {
        std::optional<string_view> state;
        std::optional<string_view> id;
        std::optional<string_view> kind;
        std::optional<string_view> progress;
        std::optional<string_view> app;
        std::optional<string_view> title;
        std::optional<string_view> msg;
    };

    /// One key the decoder reads, and where its value goes.
    struct KeySlot
    {
        string_view key;
        std::optional<string_view> RawFields::* slot;
    };

    /// The keys of program-status rev 0.4. A new key is a row here and a field in RawFields.
    constexpr auto KeySlots = std::array {
        KeySlot { .key = "state", .slot = &RawFields::state },
        KeySlot { .key = "id", .slot = &RawFields::id },
        KeySlot { .key = "kind", .slot = &RawFields::kind },
        KeySlot { .key = "progress", .slot = &RawFields::progress },
        KeySlot { .key = "app", .slot = &RawFields::app },
        KeySlot { .key = "title", .slot = &RawFields::title },
        KeySlot { .key = "msg", .slot = &RawFields::msg },
    };

    /// The limits one base64 text key is held to.
    struct TextLimits
    {
        std::size_t encoded; ///< Longest encoded value, checked before decoding.
        std::size_t decoded; ///< Longest decoded value.
    };

    constexpr auto MessageLimits = TextLimits { .encoded = 2732, .decoded = 2048 };
    constexpr auto TitleLimits = TextLimits { .encoded = 256, .decoded = 192 };

    /// Splits @p payload into its pairs and keeps each known key's last value.
    /// @return The values, or KeyTooLong when any key exceeds its limit.
    [[nodiscard]] std::expected<RawFields, ProgramStatusRejection> collectFields(string_view payload)
    {
        auto fields = RawFields {};
        for (auto const pairRange: std::views::split(payload, ':'))
        {
            auto const pair = string_view { pairRange.begin(), pairRange.end() };
            auto const separator = pair.find('=');
            if (separator == string_view::npos)
                continue; // not a pair: skipped
            auto const key = trimmed(pair.substr(0, separator));
            auto const value = trimmed(pair.substr(separator + 1));
            if (key.size() > MaxProgramStatusKeyLength)
                return std::unexpected(ProgramStatusRejection::KeyTooLong);
            if (key.empty() || !std::ranges::all_of(value, isValueByte))
                continue; // a malformed pair: skipped, the rest of the report stands
            auto const row = std::ranges::find(KeySlots, key, &KeySlot::key);
            if (row != KeySlots.end())
                fields.*(row->slot) = value; // the last value wins
        }
        return fields;
    }

    /// Whether @p id is `segment ("/" segment)*` within the length and depth limits.
    [[nodiscard]] bool isValidId(string_view id) noexcept
    {
        if (id.size() > MaxIdLength)
            return false;
        auto levels = std::size_t { 0 };
        for (auto const segment: std::views::split(id, '/'))
        {
            if (++levels > MaxIdLevels)
                return false;
            if (!isName(string_view { segment.begin(), segment.end() }, MaxIdSegmentLength))
                return false;
        }
        return true;
    }

    /// The standard base64 digits (RFC 4648 section 4), which core::base64::decode reads. Spelled here
    /// rather than borrowed from the library's detail namespace, which a re-vendor may rename.
    constexpr auto Base64Digits =
        string_view { "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/" };

    /// Whether @p text is base64: digits, then at most two `=` that complete a quartet. Padding is
    /// optional, but a lone digit in the last quartet carries no byte and is never valid.
    [[nodiscard]] bool isValidBase64(string_view text) noexcept
    {
        auto const digits = text.substr(0, text.find_first_not_of(Base64Digits));
        auto const padding = text.substr(digits.size());
        if (padding.size() > 2 || !std::ranges::all_of(padding, [](char ch) { return ch == '='; }))
            return false;
        if (digits.size() % 4 == 1)
            return false;
        return padding.empty() || (digits.size() + padding.size()) % 4 == 0;
    }

    /// Checks decoded text: UTF-8, single-line, no control character of any set.
    [[nodiscard]] std::expected<void, ProgramStatusRejection> checkText(string_view text) noexcept
    {
        auto index = std::size_t { 0 };
        while (index < text.size())
        {
            auto const lead = static_cast<unsigned char>(text[index]);
            auto length = std::size_t { 0 };
            auto codepoint = char32_t { 0 };
            auto minimum = char32_t { 0 };
            if (lead < 0x80)
            {
                length = 1;
                codepoint = lead;
            }
            else if ((lead & 0xE0) == 0xC0)
            {
                length = 2;
                codepoint = lead & 0x1Fu;
                minimum = 0x80;
            }
            else if ((lead & 0xF0) == 0xE0)
            {
                length = 3;
                codepoint = lead & 0x0Fu;
                minimum = 0x800;
            }
            else if ((lead & 0xF8) == 0xF0)
            {
                length = 4;
                codepoint = lead & 0x07u;
                minimum = 0x10000;
            }
            else
                return std::unexpected(ProgramStatusRejection::InvalidUtf8);

            if (index + length > text.size())
                return std::unexpected(ProgramStatusRejection::InvalidUtf8);
            for (auto const offset: std::views::iota(std::size_t { 1 }, length))
            {
                auto const continuation = static_cast<unsigned char>(text[index + offset]);
                if ((continuation & 0xC0) != 0x80)
                    return std::unexpected(ProgramStatusRejection::InvalidUtf8);
                codepoint = (codepoint << 6) | (continuation & 0x3Fu);
            }
            if (codepoint < minimum || codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF))
                return std::unexpected(ProgramStatusRejection::InvalidUtf8);
            if (codepoint < 0x20 || (codepoint >= 0x7F && codepoint <= 0x9F))
                return std::unexpected(ProgramStatusRejection::ControlCharacter);
            index += length;
        }
        return {};
    }

    /// Decodes one base64 text key within @p limits; an absent key is empty text.
    [[nodiscard]] std::expected<string, ProgramStatusRejection> decodeText(std::optional<string_view> raw,
                                                                           TextLimits limits)
    {
        if (!raw)
            return string {};
        if (raw->size() > limits.encoded)
            return std::unexpected(ProgramStatusRejection::TextTooLong);
        if (!isValidBase64(*raw))
            return std::unexpected(ProgramStatusRejection::BadBase64);
        auto text = core::base64::decode(*raw);
        if (text.size() > limits.decoded)
            return std::unexpected(ProgramStatusRejection::TextTooLong);
        return checkText(text).transform([&]() { return std::move(text); });
    }

    /// The enumerator @p names spells @p name, if any.
    template <typename E, std::size_t N>
    [[nodiscard]] constexpr std::optional<E> lookup(std::array<ProgramStatusName<E>, N> const& names,
                                                    string_view name) noexcept
    {
        auto const row = std::ranges::find(names, name, &ProgramStatusName<E>::name);
        if (row == names.end())
            return std::nullopt;
        return row->value;
    }

    /// `progress`, when it is a whole number 0..100.
    [[nodiscard]] std::optional<uint8_t> parseProgress(string_view text) noexcept
    {
        auto const value = core::toInteger<10, unsigned>(text); // digits only: no sign, no blanks
        if (!value || *value > MaxProgress)
            return std::nullopt;
        return static_cast<uint8_t>(*value);
    }

    /// Builds the detection reply from the name tables, so a new state or kind is one row away.
    [[nodiscard]] string buildDetectionReply()
    {
        auto const joined = [](auto const& names) {
            auto text = string {};
            for (auto const& row: names)
            {
                if (!text.empty())
                    text += ',';
                text += row.name;
            }
            return text;
        };
        return "\033]7501;?:states=" + joined(ProgramStateNames) + ":kinds=" + joined(BlockedKindNames)
               + "\033\\";
    }
} // namespace

std::expected<ProgramStatusReport, ProgramStatusRejection> parseProgramStatus(string_view payload)
{
    if (payload.size() > MaxProgramStatusPayloadLength)
        return std::unexpected(ProgramStatusRejection::TooLong);
    if (trimmed(payload) == "?")
        return ProgramStatusQuery {};

    auto const fields = collectFields(payload);
    if (!fields)
        return std::unexpected(fields.error());

    if (!fields->state)
        return std::unexpected(ProgramStatusRejection::NoState);
    auto const isClear = *fields->state == ClearState;
    auto const state = lookup(ProgramStateNames, *fields->state);
    if (!isClear && !state)
        return std::unexpected(ProgramStatusRejection::UnknownState);

    if (fields->id && !isValidId(*fields->id))
        return std::unexpected(ProgramStatusRejection::BadId);
    auto id = string { fields->id.value_or(string_view {}) };

    // The text keys are checked for a clear too: the spec discards a report that breaks a limit whole,
    // whatever it asks for.
    auto title = decodeText(fields->title, TitleLimits);
    if (!title)
        return std::unexpected(title.error());
    auto message = decodeText(fields->msg, MessageLimits);
    if (!message)
        return std::unexpected(message.error());

    if (isClear)
        return ProgramStatusClear { .id = std::move(id) };

    auto record = ProgramStatusRecord {
        .id = std::move(id),
        .state = *state,
        .kind = BlockedKind::None,
        .progress = std::nullopt,
        .app = {},
        .title = std::move(*title),
        .message = std::move(*message),
    };
    if (record.state == ProgramState::Blocked && fields->kind)
        record.kind = lookup(BlockedKindNames, *fields->kind).value_or(BlockedKind::None);
    if (takesProgress(record.state) && fields->progress)
        record.progress = parseProgress(*fields->progress);
    if (fields->app && isName(*fields->app, MaxAppLength))
        record.app = string { *fields->app };
    return record;
}

string_view programStatusDetectionReply() noexcept
{
    static auto const reply = buildDetectionReply();
    return reply;
}

} // namespace vtbackend
