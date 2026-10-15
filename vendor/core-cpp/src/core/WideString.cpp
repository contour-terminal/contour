// SPDX-License-Identifier: Apache-2.0
#include <core/WideString.hpp>

#include <cstddef>
#include <expected>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>

#include <windows.h>

// The header spells a DWORD without <windows.h>; this is where the two spellings are held equal.
static_assert(std::is_same_v<DWORD, unsigned long>);

namespace core
{

namespace
{
    /// Frees what `FormatMessageW()` allocated.
    struct LocalFreeDeleter
    {
        void operator()(wchar_t* buffer) const noexcept { LocalFree(buffer); }
    };
} // namespace

std::expected<std::wstring, std::error_code> toUtf16(std::string_view text)
{
    // MultiByteToWideChar() fails for an empty input, whose answer needs no call.
    if (text.empty())
        return std::wstring {};
    // MB_ERR_INVALID_CHARS fails the conversion where the text is not UTF-8, rather than putting
    // U+FFFD there. The view is not NUL-terminated, so each call is told its size.
    auto const length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0)
        return std::unexpected(std::make_error_code(std::errc::illegal_byte_sequence));
    auto utf16 = std::wstring(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), utf16.data(), length);
    return utf16;
}

std::string toUtf8(std::wstring_view text)
{
    if (text.empty())
        return {};
    // No flags: with WC_ERR_INVALID_CHARS a lone surrogate would fail the whole conversion, where
    // without it the surrogate reads as U+FFFD and the rest of the text survives. The view is not
    // NUL-terminated, so each call is told its size.
    auto const length = WideCharToMultiByte(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    auto utf8 = std::string(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), length, nullptr, nullptr);
    return utf8;
}

std::string systemErrorMessage(unsigned long errorCode)
{
    // Language 0 asks for the thread's UI language, then the user's, the system's and US English, so
    // a message is found wherever one is installed. A message may hold an insert, such as %1, that no
    // argument here fills, so inserts are left as they are.
    wchar_t* buffer = nullptr;
    auto const size = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
                                         | FORMAT_MESSAGE_IGNORE_INSERTS,
                                     nullptr,
                                     errorCode,
                                     0,
                                     reinterpret_cast<wchar_t*>(&buffer),
                                     0,
                                     nullptr);
    auto const owned = std::unique_ptr<wchar_t, LocalFreeDeleter> { buffer };
    if (size == 0)
        return std::format("unknown error {}", errorCode);

    // A system message ends in a line break, which a caller that puts it inside a line of its own
    // does not want.
    auto const message = std::wstring_view { owned.get(), size };
    auto const end = message.find_last_not_of(L" \t\r\n");
    return toUtf8(message.substr(0, end == std::wstring_view::npos ? 0 : end + 1));
}

} // namespace core
