// SPDX-License-Identifier: Apache-2.0
#include <core/Utf8Path.hpp>

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

#ifdef _WIN32
    #include <core/WideString.hpp>

    #include <utility>
#endif

namespace core
{

std::expected<std::filesystem::path, std::error_code> pathFromUtf8(std::string_view text)
{
#ifdef _WIN32
    // A path is UTF-16 here, so the text is converted, and strictly: not through the path's own
    // conversion of a std::u8string, which throws for text that is not UTF-8.
    return toUtf16(text).transform(
        [](std::wstring utf16) { return std::filesystem::path { std::move(utf16) }; });
#else
    // A path is the bytes it is given.
    return std::filesystem::path { std::string { text } };
#endif
}

std::string pathToUtf8(std::filesystem::path const& path)
{
#ifdef _WIN32
    // Leniently, unlike pathFromUtf8(): a name may hold a lone surrogate, and a message that names
    // it must not throw, which the path's own conversion to a std::u8string does.
    return toUtf8(path.native());
#else
    // A path is the bytes it is given.
    return path.native();
#endif
}

} // namespace core
