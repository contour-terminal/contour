// SPDX-License-Identifier: Apache-2.0
#include <core/tui/SemanticBlockClient.hpp>

#include <core/tui/InputEvent.hpp>
#include <core/tui/TerminalInput.hpp>
#include <core/tui/TerminalOutput.hpp>

#include <charconv>
#include <chrono>
#include <format>
#include <string_view>

namespace core::tui
{

SemanticBlockClient::SemanticBlockClient(TerminalOutput& output, TerminalInput& input, IClock const& clock):
    _output(output), _input(input), _clock(clock)
{
}

auto SemanticBlockClient::enable() -> bool
{
    // Send DEC mode 2034 enable: CSI ? 2034 h
    _output.writeRaw("\033[?2034h");
    _output.flush();

    // Wait for DCS token response (e.g., ESC P > 2034 ; 1 b TOKEN1;TOKEN2;TOKEN3;TOKEN4 ESC \)
    auto const response = pollForDcs(200);
    if (!response.has_value())
        return false;

    // Parse the DCS response: "> 2034 ; 1 b TOKEN_DATA"
    // The token is after the "b" byte. Format: "> PARAM ; STATUS b PAYLOAD"
    auto const& payload = *response;

    // Find the 'b' final byte that starts the payload data.
    auto const bPos = payload.find('b');
    if (bPos == std::string::npos || bPos + 1 >= payload.size())
        return false;

    _token = payload.substr(bPos + 1);
    return !_token.empty();
}

void SemanticBlockClient::disable()
{
    if (!_token.empty())
    {
        _output.writeRaw("\033[?2034l");
        _output.flush();
        _token.clear();
    }
}

auto SemanticBlockClient::isEnabled() const noexcept -> bool
{
    return !_token.empty();
}

auto SemanticBlockClient::queryLastCommand() -> SemanticBlockResult
{
    if (_token.empty())
        return { .status = SemanticBlockStatus::Unsupported, .block = std::nullopt };

    // Send semantic block query: CSI > 1 ; 1 ; TOKEN b
    // Format: ESC [ > 1 ; 1 ; T1 ; T2 ; T3 ; T4 b
    _output.writeRaw(std::format("\033[>1;1;{}b", _token));
    _output.flush();

    // Wait for DCS response.
    auto const response = pollForDcs(500);
    if (!response.has_value())
        return { .status = SemanticBlockStatus::Timeout, .block = std::nullopt };

    auto const& payload = *response;

    // Parse DCS response: "> STATUS b PAYLOAD"
    // The '>' prefix, then a status code, then 'b', then the JSON payload.
    auto const bPos = payload.find('b');
    if (bPos == std::string::npos)
        return { .status = SemanticBlockStatus::ParseError, .block = std::nullopt };

    // Extract status code: between '>' (if present) and 'b'.
    auto statusStart = size_t { 0 };
    if (!payload.empty() && payload[0] == '>')
        statusStart = 1;

    // Status is a single digit before 'b', possibly with spaces.
    auto statusCode = 0;
    auto const statusStr = std::string_view(payload).substr(statusStart, bPos - statusStart);

    // Trim spaces and parse.
    auto trimmed = statusStr;
    while (!trimmed.empty() && trimmed.front() == ' ')
        trimmed.remove_prefix(1);
    while (!trimmed.empty() && trimmed.back() == ' ')
        trimmed.remove_suffix(1);

    if (!trimmed.empty())
    {
        auto const [ptr, ec] = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), statusCode);
        if (ec != std::errc {})
            return { .status = SemanticBlockStatus::ParseError, .block = std::nullopt };
    }

    switch (statusCode)
    {
        case 0: return { .status = SemanticBlockStatus::NoData, .block = std::nullopt };
        case 2: return { .status = SemanticBlockStatus::AuthRequired, .block = std::nullopt };
        case 3: return { .status = SemanticBlockStatus::AuthFailed, .block = std::nullopt };
        case 1: {
            // Success — parse JSON payload after 'b'.
            auto const jsonPayload = std::string_view(payload).substr(bPos + 1);
            if (jsonPayload.empty())
                return { .status = SemanticBlockStatus::ParseError, .block = std::nullopt };

            auto block = parseBlockJson(jsonPayload);
            if (!block.has_value())
                return { .status = SemanticBlockStatus::ParseError, .block = std::nullopt };

            return { .status = SemanticBlockStatus::Success, .block = std::move(block) };
        }
        default: return { .status = SemanticBlockStatus::ParseError, .block = std::nullopt };
    }
}

auto SemanticBlockClient::parseBlockJson(std::string_view json) -> std::optional<CommandBlockInfo>
{
    // Minimal JSON parser for the fixed schema. The response has a known structure:
    // {"version":1,"command":"...", "output":"...", "exitCode":N, "finished":bool, "outputLineCount":N}
    // We parse this without an external JSON library.

    auto info = CommandBlockInfo {};

    auto findStringValue = [&](std::string_view key) -> std::optional<std::string> {
        auto const keyStr = std::format("\"{}\"", key);
        auto const pos = json.find(keyStr);
        if (pos == std::string_view::npos)
            return std::nullopt;

        // Find the colon after the key.
        auto colonPos = json.find(':', pos + keyStr.size());
        if (colonPos == std::string_view::npos)
            return std::nullopt;

        // Find the opening quote of the value.
        auto quoteStart = json.find('"', colonPos + 1);
        if (quoteStart == std::string_view::npos)
            return std::nullopt;

        // Find the closing quote, handling escaped quotes.
        auto result = std::string {};
        auto i = quoteStart + 1;
        while (i < json.size())
        {
            if (json[i] == '\\' && i + 1 < json.size())
            {
                switch (json[i + 1])
                {
                    case '"': result += '"'; break;
                    case '\\': result += '\\'; break;
                    case 'n': result += '\n'; break;
                    case 'r': result += '\r'; break;
                    case 't': result += '\t'; break;
                    default: result += json[i + 1]; break;
                }
                i += 2;
            }
            else if (json[i] == '"')
            {
                return result;
            }
            else
            {
                result += json[i];
                ++i;
            }
        }
        return std::nullopt; // Unterminated string.
    };

    auto findIntValue = [&](std::string_view key) -> std::optional<int> {
        auto const keyStr = std::format("\"{}\"", key);
        auto const pos = json.find(keyStr);
        if (pos == std::string_view::npos)
            return std::nullopt;

        auto colonPos = json.find(':', pos + keyStr.size());
        if (colonPos == std::string_view::npos)
            return std::nullopt;

        // Skip whitespace after colon.
        auto start = colonPos + 1;
        while (start < json.size() && json[start] == ' ')
            ++start;

        auto value = 0;
        auto negative = false;
        if (start < json.size() && json[start] == '-')
        {
            negative = true;
            ++start;
        }

        auto const [ptr, ec] = std::from_chars(json.data() + start, json.data() + json.size(), value);
        if (ec != std::errc {})
            return std::nullopt;

        return negative ? -value : value;
    };

    auto findBoolValue = [&](std::string_view key) -> std::optional<bool> {
        auto const keyStr = std::format("\"{}\"", key);
        auto const pos = json.find(keyStr);
        if (pos == std::string_view::npos)
            return std::nullopt;

        auto colonPos = json.find(':', pos + keyStr.size());
        if (colonPos == std::string_view::npos)
            return std::nullopt;

        auto rest = json.substr(colonPos + 1);
        while (!rest.empty() && rest.front() == ' ')
            rest.remove_prefix(1);

        if (rest.starts_with("true"))
            return true;
        if (rest.starts_with("false"))
            return false;
        return std::nullopt;
    };

    if (auto cmd = findStringValue("command"))
        info.command = std::move(*cmd);

    if (auto out = findStringValue("output"))
        info.output = std::move(*out);

    if (auto code = findIntValue("exitCode"))
        info.exitCode = *code;

    if (auto fin = findBoolValue("finished"))
        info.finished = *fin;

    if (auto count = findIntValue("outputLineCount"))
        info.outputLineCount = *count;

    return info;
}

auto SemanticBlockClient::pollForDcs(int timeoutMs) -> std::optional<std::string>
{
    auto const deadline = _clock.now() + std::chrono::milliseconds(timeoutMs);

    while (true)
    {
        auto const now = _clock.now();
        if (now >= deadline)
            break;

        auto const remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        auto events = _input.poll(static_cast<int>(remaining));

        if (events.empty())
            break;

        for (auto const& event: events)
        {
            if (auto const* dcs = std::get_if<DcsResponse>(&event))
                return dcs->payload;
        }
    }

    return std::nullopt;
}

} // namespace core::tui
