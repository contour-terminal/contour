// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>
#include <string_view>

namespace core::platform
{

/// @brief Percent-encodes @p input per RFC 3986.
///
/// Every byte outside the unreserved set (`A-Za-z0-9` plus `-._~`) and outside @p extraSafe is
/// replaced by an uppercase `%XX` triplet. Encoding operates on raw bytes, so UTF-8 text is
/// encoded per RFC 3986 (`ä` becomes `%C3%A4`).
///
/// Byte classification uses a compile-time table rather than `std::isalnum`, which is
/// locale-dependent and would leave high bytes unencoded under some locales.
///
/// @param input Text to encode.
/// @param extraSafe Additional bytes to pass through unencoded, e.g. `/` for a path component.
///        Keep it short: membership is a linear scan per input byte.
/// @return The percent-encoded text.
[[nodiscard]] auto percentEncode(std::string_view input, std::string_view extraSafe = {}) -> std::string;

/// @brief Appends percentEncode(@p input, @p extraSafe) to @p out.
///
/// The in-place form, for callers assembling a URI piecewise — a URI built from several encoded
/// parts otherwise allocates a throwaway string per part.
///
/// @param out Destination to append to.
/// @param input Text to encode.
/// @param extraSafe Additional bytes to pass through unencoded.
void appendPercentEncoded(std::string& out, std::string_view input, std::string_view extraSafe = {});

/// @brief Percent-encodes a filesystem path for the path component of a URI.
///
/// percentEncode() with `/` kept as the separator.
///
/// @param path Path with `/` separators (see normalizePath()).
/// @return The percent-encoded path.
[[nodiscard]] auto percentEncodeUriPath(std::string_view path) -> std::string;

/// @brief Appends percentEncodeUriPath(@p path) to @p out.
/// @param out Destination to append to.
/// @param path Path with `/` separators (see normalizePath()).
void appendPercentEncodedUriPath(std::string& out, std::string_view path);

/// @brief Builds an RFC 8089 `file://` URI for an absolute filesystem path.
///
/// Examples:
/// @code
/// fileUri("/home/me/My Docs", "box")  == "file://box/home/me/My%20Docs"
/// fileUri("/tmp/x", "")               == "file:///tmp/x"
/// fileUri("C:/Users/me", "box")       == "file://box/C:/Users/me"
/// fileUri("//srv/share/x", "box")     == "file://srv/share/x"
/// fileUri("/a/b.cpp", "box", "42")    == "file://box/a/b.cpp#42"
/// @endcode
///
/// @param absolutePath Absolute path with `/` separators. A Windows drive specifier gains
///        the leading slash the scheme requires; the drive colon is left literal because
///        that is what terminals expect.
/// @param host Authority component, normally hostName(). An empty host yields the local
///        `file:///path` form, which RFC 8089 defines as equivalent to localhost. A UNC
///        path takes its authority from the path itself and ignores @p host.
/// @param fragment Optional fragment appended as `#fragment`, e.g. a line number. Encoded
///        with the same rules as the path.
/// @return The complete URI, or an empty string when @p absolutePath is empty.
[[nodiscard]] auto fileUri(std::string_view absolutePath,
                           std::string_view host,
                           std::string_view fragment = {}) -> std::string;

} // namespace core::platform
