// SPDX-License-Identifier: Apache-2.0
#include <core/tui/ClipboardProtocol.hpp>

#include <core/Base64.hpp>
#include <core/tui/TerminalProtocols.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>

namespace core::tui
{

namespace
{
    using namespace std::string_view_literals;

    /// @brief How each selection is named in the two protocols.
    struct TargetRow
    {
        ClipboardTarget target;
        std::string_view osc52Selector;   ///< OSC 52's selection parameter.
        std::string_view osc5522Location; ///< The metadata OSC 5522's write header carries.
    };

    constexpr auto TargetTable = std::array {
        TargetRow { .target = ClipboardTarget::Clipboard, .osc52Selector = "c"sv, .osc5522Location = ""sv },
        TargetRow {
            .target = ClipboardTarget::Primary, .osc52Selector = "p"sv, .osc5522Location = ":loc=primary"sv },
    };

    /// @brief One error: the OSC 5522 status code that reports it, if any, and its description.
    struct ErrorRow
    {
        ClipboardWriteError error;
        std::string_view wireCode; ///< Empty for an error no terminal reports.
        std::string_view message;
    };

    constexpr auto ErrorTable = std::array {
        ErrorRow { .error = ClipboardWriteError::NoTerminal,
                   .wireCode = ""sv,
                   .message = "no controlling terminal to send the clipboard sequence to"sv },
        ErrorRow { .error = ClipboardWriteError::UnsupportedMimeType,
                   .wireCode = ""sv,
                   .message = "terminal only supports OSC 52 (plain text); cannot copy this MIME type"sv },
        ErrorRow { .error = ClipboardWriteError::PermissionDenied,
                   .wireCode = "EPERM"sv,
                   .message = "terminal refused clipboard access (EPERM)"sv },
        ErrorRow { .error = ClipboardWriteError::TooLarge,
                   .wireCode = "EFBIG"sv,
                   .message = "data too large for the terminal's clipboard (EFBIG)"sv },
        ErrorRow { .error = ClipboardWriteError::Busy,
                   .wireCode = "EBUSY"sv,
                   .message = "terminal clipboard busy (EBUSY)"sv },
        ErrorRow { .error = ClipboardWriteError::InvalidData,
                   .wireCode = "EINVAL"sv,
                   .message = "terminal rejected the clipboard data (EINVAL)"sv },
        ErrorRow { .error = ClipboardWriteError::IoError,
                   .wireCode = "EIO"sv,
                   .message = "terminal reported an I/O error, or writing to it failed"sv },
        ErrorRow { .error = ClipboardWriteError::PrimaryUnavailable,
                   .wireCode = "ENOSYS"sv,
                   .message = "terminal has no primary selection (ENOSYS)"sv },
        ErrorRow { .error = ClipboardWriteError::NoConfirmation,
                   .wireCode = ""sv,
                   .message = "terminal did not confirm the copy in time"sv },
    };

    constexpr auto OscIntroducer = "\033]"sv;
    constexpr auto Osc5522Prefix = "5522;"sv;
    using protocols::StringTerminator;

    static_assert(Osc5522Probe.ends_with(protocols::QueryPrimaryDeviceAttributes));

    auto targetRow(ClipboardTarget target) noexcept -> TargetRow const&
    {
        return *std::ranges::find(TargetTable, target, &TargetRow::target);
    }

    /// @brief The value of @p key in OSC 5522 metadata (`key=value:key=value`), if present.
    auto metadataValue(std::string_view metadata, std::string_view key) -> std::optional<std::string_view>
    {
        for (auto const field: metadata | std::views::split(':'))
        {
            auto const pair = std::string_view { field.begin(), field.end() };
            auto const eq = pair.find('=');
            if (eq != std::string_view::npos && pair.substr(0, eq) == key)
                return pair.substr(eq + 1);
        }
        return std::nullopt;
    }

    /// @brief How many characters base64 turns @p size bytes into.
    constexpr auto base64Size(std::size_t size) noexcept -> std::size_t
    {
        return ((size + 2) / 3) * 4;
    }

    /// @brief Appends the base64 of @p data to @p out, without a temporary.
    void appendBase64(std::string& out, std::string_view data)
    {
        auto state = base64::EncoderState {};
        auto const sink = [&out](char a, char b, char c, char d) {
            out += a;
            out += b;
            out += c;
            out += d;
        };
        for (auto const ch: data)
            base64::encode(static_cast<std::uint8_t>(ch), state, sink);
        base64::finish(state, sink);
    }

    /// @brief Appends an OSC 5522 packet that carries no payload.
    void appendOsc5522Control(std::string& out, std::string_view metadata)
    {
        out += OscIntroducer;
        out += Osc5522Prefix;
        out += metadata;
        out += StringTerminator;
    }

} // namespace

void appendOsc52(std::string& out, std::string_view data, ClipboardTarget target)
{
    out.reserve(out.size() + OscIntroducer.size() + 5 + base64Size(data.size()) + StringTerminator.size());
    out += OscIntroducer;
    out += "52;";
    out += targetRow(target).osc52Selector;
    out += ';';
    appendBase64(out, data);
    out += StringTerminator;
}

auto encodeOsc52(std::string_view data, ClipboardTarget target) -> std::string
{
    auto out = std::string {};
    appendOsc52(out, data, target);
    return out;
}

auto encodeOsc5522Write(std::string_view data, std::string_view mime, ClipboardTarget target) -> std::string
{
    auto dataHeader = std::string { OscIntroducer };
    dataHeader += Osc5522Prefix;
    dataHeader += "type=wdata:mime=";
    appendBase64(dataHeader, mime);
    dataHeader += ';';

    // Empty data is still one packet, so the MIME type is declared. Offsets rather than
    // std::views::chunk, which Apple's libc++ does not have yet.
    auto const chunkCount = std::max<std::size_t>(1, (data.size() + Osc5522ChunkSize - 1) / Osc5522ChunkSize);

    auto out = std::string {};
    out.reserve(64 + (chunkCount * (dataHeader.size() + StringTerminator.size())) + base64Size(data.size()));
    appendOsc5522Control(out, std::string { "type=write" }.append(targetRow(target).osc5522Location));
    for (auto const index: std::views::iota(std::size_t { 0 }, chunkCount))
    {
        out += dataHeader;
        appendBase64(out, data.substr(std::min(index * Osc5522ChunkSize, data.size()), Osc5522ChunkSize));
        out += StringTerminator;
    }
    appendOsc5522Control(out, "type=wdata");
    return out;
}

auto parseOsc5522WriteStatus(std::string_view oscPayload)
    -> std::optional<std::expected<void, ClipboardWriteError>>
{
    if (!oscPayload.starts_with(Osc5522Prefix))
        return std::nullopt;

    auto const metadata = oscPayload.substr(Osc5522Prefix.size());
    auto const status = metadataValue(metadata, "status");
    if (metadataValue(metadata, "type") != "write" || !status)
        return std::nullopt;

    if (*status == "DONE")
        return std::expected<void, ClipboardWriteError> {};

    auto const row = std::ranges::find(ErrorTable, *status, &ErrorRow::wireCode);
    auto const error = row != ErrorTable.end() ? row->error : ClipboardWriteError::IoError;
    return std::expected<void, ClipboardWriteError> { std::unexpect, error };
}

auto describe(ClipboardWriteError error) noexcept -> std::string_view
{
    return std::ranges::find(ErrorTable, error, &ErrorRow::error)->message;
}

auto isPlainTextMimeType(std::string_view mime) noexcept -> bool
{
    constexpr auto PlainText = "text/plain"sv;
    return mime == PlainText
           || (mime.starts_with(PlainText) && mime.substr(PlainText.size()).starts_with(';'));
}

} // namespace core::tui
