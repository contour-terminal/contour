// SPDX-License-Identifier: Apache-2.0
#include <contour/input/Logging.hpp>
#include <contour/input/NativeKeyEvent.hpp>

#include <QtCore/QString>

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <ranges>

namespace contour::input
{

namespace
{
    struct TypeName
    {
        vtbackend::KeyboardEventType type;
        std::string_view name;
    };

    constexpr auto TypeNames = std::array {
        TypeName { vtbackend::KeyboardEventType::Press, "press" },
        TypeName { vtbackend::KeyboardEventType::Repeat, "repeat" },
        TypeName { vtbackend::KeyboardEventType::Release, "release" },
    };

    struct OriginName
    {
        KeyEventOrigin origin;
        std::string_view name;
    };

    constexpr auto OriginNames = std::array {
        OriginName { KeyEventOrigin::Keyboard, "keyboard" },
        OriginName { KeyEventOrigin::InputMethodCommit, "ime" },
    };

    constexpr std::string_view HexDigits = "0123456789abcdef";

    /// Escapes @p utf8 so it holds no control character, backslash or space.
    [[nodiscard]] std::string escapeText(std::string_view utf8)
    {
        auto out = std::string {};
        for (auto const ch: utf8)
        {
            auto const byte = static_cast<unsigned char>(ch);
            if (byte < 0x20 || byte == 0x7F || byte == ' ' || byte == '\\')
            {
                out += "\\x";
                out += HexDigits[byte >> 4];
                out += HexDigits[byte & 0x0F];
            }
            else
                out += ch;
        }
        return out;
    }

    [[nodiscard]] std::optional<std::string> unescapeText(std::string_view escaped)
    {
        auto out = std::string {};
        for (size_t i = 0; i < escaped.size(); ++i)
        {
            if (escaped[i] != '\\')
            {
                out += escaped[i];
                continue;
            }
            if (i + 4 > escaped.size())
                return std::nullopt;
            if (escaped[i + 1] != 'x')
                return std::nullopt;
            auto value = unsigned {};
            auto const digits = escaped.substr(i + 2, 2);
            auto const [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), value, 16);
            if (ec != std::errc {} || end != digits.data() + digits.size())
                return std::nullopt;
            out += static_cast<char>(value);
            i += 3;
        }
        return out;
    }

    /// Takes the next space-separated `name=value` token off @p line.
    /// @return The value when the next token is named @p name, else nullopt.
    [[nodiscard]] std::optional<std::string_view> takeField(std::string_view& line, std::string_view name)
    {
        if (!line.starts_with(name) || line.size() <= name.size() || line[name.size()] != '=')
            return std::nullopt;
        line.remove_prefix(name.size() + 1);
        auto const end = std::min(line.find(' '), line.size());
        auto const value = line.substr(0, end);
        line.remove_prefix(std::min(end + 1, line.size()));
        return value;
    }

    [[nodiscard]] std::optional<uint32_t> parseHex(std::string_view token)
    {
        if (!token.starts_with("0x") || token.size() == 2)
            return std::nullopt;
        token.remove_prefix(2);
        auto value = uint32_t {};
        auto const [end, ec] = std::from_chars(token.data(), token.data() + token.size(), value, 16);
        if (ec != std::errc {} || end != token.data() + token.size())
            return std::nullopt;
        return value;
    }

    template <typename Row, typename Value>
    [[nodiscard]] std::optional<Value> lookUp(std::string_view name, auto const& rows, Value Row::* member)
    {
        auto const i = std::ranges::find(rows, name, &Row::name);
        if (i == rows.end())
            return std::nullopt;
        return (*i).*member;
    }
} // namespace

std::string formatRecorded(NativeKeyEvent const& event)
{
    auto const typeName = std::ranges::find(TypeNames, event.type, &TypeName::type)->name;
    auto const originName = std::ranges::find(OriginNames, event.origin, &OriginName::origin)->name;
    auto const utf8 = QString::fromStdU32String(event.text).toUtf8();
    return std::format("key=0x{:x} mods=0x{:x} native=0x{:x} vk=0x{:x} scan=0x{:x} type={} origin={} text={}",
                       static_cast<uint32_t>(event.qtKey),
                       event.qtModifiers,
                       event.nativeModifiers,
                       event.nativeVirtualKey,
                       event.nativeScanCode,
                       typeName,
                       originName,
                       escapeText(std::string_view { utf8.constData(), static_cast<size_t>(utf8.size()) }));
}

std::optional<NativeKeyEvent> parseRecorded(std::string_view line)
{
    auto event = NativeKeyEvent {};

    auto const key = takeField(line, "key").and_then(parseHex);
    auto const mods = takeField(line, "mods").and_then(parseHex);
    auto const native = takeField(line, "native").and_then(parseHex);
    auto const vk = takeField(line, "vk").and_then(parseHex);
    auto const scan = takeField(line, "scan").and_then(parseHex);
    auto const type =
        takeField(line, "type").and_then([](auto name) { return lookUp(name, TypeNames, &TypeName::type); });
    auto const origin = takeField(line, "origin").and_then([](auto name) {
        return lookUp(name, OriginNames, &OriginName::origin);
    });
    if (!key || !mods || !native || !vk || !scan || !type || !origin)
        return std::nullopt;

    if (!line.starts_with("text="))
        return std::nullopt;
    auto const text = line.substr(5);
    if (text.contains(' '))
        return std::nullopt;
    auto const utf8 = unescapeText(text);
    if (!utf8)
        return std::nullopt;

    event.qtKey = static_cast<int>(*key);
    event.qtModifiers = *mods;
    event.nativeModifiers = *native;
    event.nativeVirtualKey = *vk;
    event.nativeScanCode = *scan;
    event.type = *type;
    event.origin = *origin;
    event.text = QString::fromUtf8(utf8->data(), static_cast<qsizetype>(utf8->size())).toStdU32String();
    return event;
}

void logRecorded(NativeKeyEvent const& event)
{
    if (!inputLog.isEnabled())
        return;
    inputLog()("Captured {}", formatRecorded(event));
}

} // namespace contour::input
