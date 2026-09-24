// SPDX-License-Identifier: Apache-2.0
#include <core/platform/windows/WindowsProcessEnvironment.hpp>

#include <core/Environment.hpp>

#include <algorithm>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <windows.h>

namespace core::platform
{

namespace
{
    /// @return What the process environment's writer answered, as this interface spells it.
    [[nodiscard]] std::expected<void, PlatformError> toPlatformResult(
        std::expected<void, std::error_code> result)
    {
        return result.transform_error([](std::error_code const& error) {
            return error == std::errc::invalid_argument ? PlatformError::InvalidArgument
                                                        : PlatformError::IoError;
        });
    }

    /// @return @p text in UTF-8, as every name core-cpp hands back is spelled.
    [[nodiscard]] std::string toUtf8(std::wstring const& text)
    {
        if (text.empty())
            return {};
        auto const size = static_cast<int>(text.size());
        auto const length = WideCharToMultiByte(CP_UTF8, 0, text.data(), size, nullptr, 0, nullptr, nullptr);
        auto utf8 = std::string(static_cast<std::size_t>(length), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.data(), size, utf8.data(), length, nullptr, nullptr);
        return utf8;
    }

    /// @return @p text in UTF-16, for the comparison below. A byte that is not UTF-8 becomes
    ///         U+FFFD: such a name cannot reach the process environment anyway.
    [[nodiscard]] std::wstring toWide(std::string const& text)
    {
        if (text.empty())
            return {};
        auto const size = static_cast<int>(text.size());
        auto const length = MultiByteToWideChar(CP_UTF8, 0, text.data(), size, nullptr, 0);
        auto wide = std::wstring(static_cast<std::size_t>(length), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), size, wide.data(), length);
        return wide;
    }

    /// Compares two variable names as Windows does: ordinally, with case folded through the
    /// operating system's own upper-case table, in all of Unicode and not only ASCII.
    /// @return Negative, zero or positive, as @p a sorts before, with or after @p b.
    [[nodiscard]] int compareNames(std::string_view a, std::string_view b)
    {
        auto const wideA = toWide(std::string { a });
        auto const wideB = toWide(std::string { b });
        auto const order = ::CompareStringOrdinal(wideA.c_str(),
                                                  static_cast<int>(wideA.size()),
                                                  wideB.c_str(),
                                                  static_cast<int>(wideB.size()),
                                                  TRUE);
        return order - CSTR_EQUAL;
    }

    /// @return Whether @p a and @p b name the same variable.
    [[nodiscard]] bool sameName(std::string_view a, std::string_view b)
    {
        return compareNames(a, b) == 0;
    }
} // namespace

bool WindowsProcessEnvironment::CaseInsensitiveLess::operator()(std::string_view a, std::string_view b) const
{
    return compareNames(a, b) < 0;
}

std::unique_ptr<ProcessEnvironment> nativeProcessEnvironment()
{
    return std::make_unique<WindowsProcessEnvironment>();
}

std::expected<void, PlatformError> WindowsProcessEnvironment::set(std::string_view name,
                                                                  std::string_view value)
{
    if (!isValidEnvironmentName(name) || value.contains('\0'))
        return std::unexpected(PlatformError::InvalidArgument);
    _values.insert_or_assign(std::string { name }, std::string { value });
    return {};
}

std::optional<std::string> WindowsProcessEnvironment::get(std::string_view name) const
{
    if (auto const it = _values.find(name); it != _values.end())
        return it->second;

    // core::LiveEnvironment reads the same Win32 block, and reads it right: a return of 0 from
    // GetEnvironmentVariableW is an empty value -- a variable that is set -- unless the call says
    // the name is gone, a value that does not fit the buffer needs the size it reports, and the
    // block is UTF-16, which it converts. The copy that used to stand here got the first two wrong
    // and read through the ANSI code page, so the two readers disagreed about the same block.
    return core::LiveEnvironment {}.get(name);
}

std::expected<void, PlatformError> WindowsProcessEnvironment::unset(std::string_view name)
{
    if (!isValidEnvironmentName(name))
        return std::unexpected(PlatformError::InvalidArgument);
    if (auto const it = _values.find(name); it != _values.end())
        _values.erase(it);
    return toPlatformResult(core::unsetProcessEnvironmentVariable(name));
}

std::expected<void, PlatformError> WindowsProcessEnvironment::exportVariable(std::string_view name)
{
    if (!isValidEnvironmentName(name))
        return std::unexpected(PlatformError::InvalidArgument);
    if (auto const it = _values.find(name); it != _values.end())
        return toPlatformResult(core::setProcessEnvironmentVariable(it->first, it->second));
    return {};
}

std::vector<std::string> WindowsProcessEnvironment::keys() const
{
    std::vector<std::string> result;

    auto const envBlock = GetEnvironmentStringsW();
    if (envBlock != nullptr)
    {
        auto const* p = envBlock;
        while (*p != L'\0')
        {
            auto const entry = std::wstring_view(p);
            // Converted, not narrowed a code unit at a time: a name outside ASCII lost every high
            // byte that way (core-cpp#7).
            if (auto const eq = entry.find(L'='); eq != std::wstring_view::npos && eq > 0)
                result.push_back(toUtf8(std::wstring { entry.substr(0, eq) }));
            p += entry.size() + 1;
        }
        FreeEnvironmentStringsW(envBlock);
    }

    for (auto const& [key, _]: _values)
    {
        if (std::ranges::none_of(result,
                                 [&](std::string const& existing) { return sameName(existing, key); }))
            result.push_back(key);
    }

    return result;
}

} // namespace core::platform
