// SPDX-License-Identifier: Apache-2.0
#include <contour/askpass/AskpassProtocol.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <ranges>
#include <utility>

namespace contour::askpass
{

namespace
{
    constexpr auto LengthBytes = AskpassFramePrefixSize;

    void appendLength(std::string& out, std::size_t value)
    {
        for (auto const shift: std::views::iota(0U, 4U))
            out.push_back(static_cast<char>((value >> (shift * 8U)) & 0xFFU));
    }

    void appendField(std::string& out, std::string_view field)
    {
        appendLength(out, field.size());
        out.append(field);
    }

    /// Wraps @p payload in its length prefix.
    [[nodiscard]] std::string framed(std::string_view payload)
    {
        auto out = std::string {};
        out.reserve(LengthBytes + payload.size());
        appendLength(out, payload.size());
        out.append(payload);
        return out;
    }

    [[nodiscard]] std::size_t readLength(std::string_view bytes) noexcept
    {
        auto value = std::size_t { 0 };
        for (auto const shift: std::views::iota(0U, 4U))
            value |= static_cast<std::size_t>(static_cast<unsigned char>(bytes[shift])) << (shift * 8U);
        return value;
    }

    /// A cursor over a payload that refuses to read past its end.
    class Reader
    {
      public:
        explicit Reader(std::string_view payload) noexcept: _rest { payload } {}

        [[nodiscard]] std::optional<char> byte() noexcept
        {
            if (_rest.empty())
                return std::nullopt;
            auto const value = _rest.front();
            _rest.remove_prefix(1);
            return value;
        }

        [[nodiscard]] std::optional<std::string> field()
        {
            if (_rest.size() < LengthBytes)
                return std::nullopt;
            auto const size = readLength(_rest);
            _rest.remove_prefix(LengthBytes);
            if (_rest.size() < size)
                return std::nullopt;
            auto value = std::string { _rest.substr(0, size) };
            _rest.remove_prefix(size);
            return value;
        }

        [[nodiscard]] bool atEnd() const noexcept { return _rest.empty(); }

      private:
        std::string_view _rest;
    };

    /// @return The payload of the one whole frame @p frame is, or why it is not.
    [[nodiscard]] std::expected<std::string_view, std::string> payloadOf(std::string_view frame)
    {
        auto const size = askpassFrameSize(frame);
        if (!size)
            return std::unexpected { size.error() };
        if (!size->has_value())
            return std::unexpected { std::string { "askpass frame is truncated" } };
        if (**size != frame.size())
            return std::unexpected { std::string {
                **size > frame.size() ? "askpass frame is truncated" : "askpass frame has trailing bytes" } };
        return frame.substr(LengthBytes);
    }
} // namespace

AskpassPromptKind classifyAskpassPrompt(std::string_view prompt, std::string_view sshAskpassPrompt)
{
    for (auto const& rule: AskpassPromptRules)
    {
        auto const matches = rule.field == AskpassPromptRule::Field::SshAskpassPrompt
                                 ? sshAskpassPrompt == rule.needle
                                 : prompt.contains(rule.needle);
        if (matches)
            return rule.kind;
    }
    return AskpassPromptKind::Secret;
}

std::string encode(AskpassRequest const& request)
{
    auto payload = std::string {};
    appendField(payload, request.nonce);
    appendField(payload, request.prompt);
    appendField(payload, request.sshAskpassPrompt);
    return framed(payload);
}

std::string encode(AskpassReply const& reply)
{
    auto payload = std::string {};
    payload.push_back(static_cast<char>(std::to_underlying(reply.outcome)));
    appendField(payload, reply.answer);
    return framed(payload);
}

std::expected<std::optional<std::size_t>, std::string> askpassFrameSize(std::string_view buffered)
{
    if (buffered.size() < LengthBytes)
        return std::optional<std::size_t> {};
    auto const payload = readLength(buffered);
    if (payload > MaxAskpassPayload)
        return std::unexpected { std::format(
            "askpass frame of {} bytes exceeds the {} byte limit", payload, MaxAskpassPayload) };
    return std::optional<std::size_t> { LengthBytes + payload };
}

std::expected<AskpassRequest, std::string> decodeAskpassRequest(std::string_view frame)
{
    auto const payload = payloadOf(frame);
    if (!payload)
        return std::unexpected { payload.error() };
    auto reader = Reader { *payload };
    auto nonce = reader.field();
    auto prompt = reader.field();
    auto sshAskpassPrompt = reader.field();
    if (!nonce || !prompt || !sshAskpassPrompt || !reader.atEnd())
        return std::unexpected { std::string { "askpass request is malformed" } };
    return AskpassRequest { .nonce = *std::move(nonce),
                            .prompt = *std::move(prompt),
                            .sshAskpassPrompt = *std::move(sshAskpassPrompt) };
}

std::expected<AskpassReply, std::string> decodeAskpassReply(std::string_view frame)
{
    auto const payload = payloadOf(frame);
    if (!payload)
        return std::unexpected { payload.error() };
    auto reader = Reader { *payload };
    auto const outcome = reader.byte();
    auto answer = reader.field();
    if (!outcome || !answer || !reader.atEnd())
        return std::unexpected { std::string { "askpass reply is malformed" } };
    auto const value = static_cast<std::uint8_t>(*outcome);
    if (value > std::to_underlying(AskpassOutcome::Cancelled))
        return std::unexpected { std::string { "askpass reply has an unknown outcome" } };
    return AskpassReply { .outcome = static_cast<AskpassOutcome>(value), .answer = *std::move(answer) };
}

void wipe(std::string& secret) noexcept
{
    for (auto& character: secret)
        static_cast<char volatile&>(character) = '\0';
    secret.clear();
}

} // namespace contour::askpass
