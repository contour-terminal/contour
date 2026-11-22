// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Which links a daemon extension may have a frontend open (`url-open`, PDU OpenUrl): one rule, applied by
/// the daemon before it sends one and by the client before it opens one.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <ranges>
#include <string_view>
#include <utility>

namespace vthost
{

/// The longest link `url-open` takes, in bytes.
inline constexpr std::size_t MaxUrlToOpenBytes = 2048;

/// Why a link is not opened.
enum class UrlRefusal : uint8_t
{
    TooLong = 0, ///< It is longer than MaxUrlToOpenBytes.
    NotWeb,      ///< Its scheme is neither http nor https.
    NoHost,      ///< It names no host.
    Disguised,   ///< It holds a control character or a blank, which can hide what it opens.
};

/// One refusal and how it is said.
struct UrlRefusalRow
{
    UrlRefusal refusal;     ///< The refusal.
    std::string_view words; ///< What a log says, after "not opening the link: ".
};

/// One row per UrlRefusal, indexed by its value.
inline constexpr auto UrlRefusalRows = std::array {
    UrlRefusalRow { .refusal = UrlRefusal::TooLong, .words = "it is longer than 2 KiB" },
    UrlRefusalRow { .refusal = UrlRefusal::NotWeb, .words = "only http and https links open" },
    UrlRefusalRow { .refusal = UrlRefusal::NoHost, .words = "it names no host" },
    UrlRefusalRow { .refusal = UrlRefusal::Disguised, .words = "it holds a control character or a blank" },
};

static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, UrlRefusalRows.size()),
                                  [](std::size_t index) {
                                      return std::to_underlying(UrlRefusalRows.at(index).refusal) == index;
                                  }),
              "UrlRefusalRows is indexed by the enumerator's value");

/// @param refusal Why a link is not opened.
/// @return How it is said.
[[nodiscard]] constexpr std::string_view wordsOf(UrlRefusal refusal) noexcept
{
    return UrlRefusalRows.at(std::to_underlying(refusal)).words;
}

/// The schemes a link may have, as they are written before `://`, in lower case.
inline constexpr auto WebSchemes = std::array { std::string_view { "https" }, std::string_view { "http" } };

/// @param url A link a daemon extension asked to open.
/// @return Nothing when it may be opened: an `http://` or `https://` URL (the scheme in any case) that names
///         a host, at most MaxUrlToOpenBytes, without a control character or a blank; else why not.
[[nodiscard]] constexpr std::expected<void, UrlRefusal> checkUrlToOpen(std::string_view url) noexcept
{
    if (url.size() > MaxUrlToOpenBytes)
        return std::unexpected(UrlRefusal::TooLong);
    auto const separator = url.find("://");
    if (separator == std::string_view::npos)
        return std::unexpected(UrlRefusal::NotWeb);
    auto const scheme = url.substr(0, separator);
    auto const isWeb = std::ranges::any_of(WebSchemes, [&](std::string_view web) {
        return std::ranges::equal(scheme, web, [](char a, char b) {
            return (a >= 'A' && a <= 'Z' ? static_cast<char>(a - 'A' + 'a') : a) == b;
        });
    });
    if (!isWeb)
        return std::unexpected(UrlRefusal::NotWeb);
    if (std::ranges::any_of(url, [](char c) {
            auto const byte = static_cast<unsigned char>(c);
            return byte <= 0x20 || byte == 0x7F;
        }))
        return std::unexpected(UrlRefusal::Disguised);
    auto const rest = url.substr(separator + 3);
    if (rest.empty() || rest.front() == '/' || rest.front() == '?' || rest.front() == '#')
        return std::unexpected(UrlRefusal::NoHost);
    return {};
}

} // namespace vthost
