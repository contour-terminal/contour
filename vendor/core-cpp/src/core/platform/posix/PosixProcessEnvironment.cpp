// SPDX-License-Identifier: Apache-2.0
#include <core/platform/posix/PosixProcessEnvironment.hpp>

#include <core/Environment.hpp>

#include <algorithm>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <unistd.h>

// glibc's <unistd.h> declares `environ` (C++ builds define _GNU_SOURCE); macOS reaches it
// through _NSGetEnviron(), and the other systems want it declared here.
#ifdef __APPLE__
    #include <crt_externs.h>
#elifndef __GLIBC__
extern "C" char** environ;
#endif

namespace core::platform
{

namespace
{
    /// @return The process's environment block, as child processes inherit it.
    [[nodiscard]] char** processEnviron() noexcept
    {
#ifdef __APPLE__
        return *_NSGetEnviron();
#else
        return environ;
#endif
    }

    /// @return What the process environment's writer answered, as this interface spells it.
    [[nodiscard]] std::expected<void, PlatformError> toPlatformResult(
        std::expected<void, std::error_code> result)
    {
        return result.transform_error([](std::error_code const& error) {
            return error == std::errc::invalid_argument ? PlatformError::InvalidArgument
                                                        : PlatformError::IoError;
        });
    }
} // namespace

std::unique_ptr<ProcessEnvironment> nativeProcessEnvironment()
{
    return std::make_unique<PosixProcessEnvironment>();
}

std::expected<void, PlatformError> PosixProcessEnvironment::set(std::string_view name, std::string_view value)
{
    if (!isValidEnvironmentName(name) || value.contains('\0'))
        return std::unexpected(PlatformError::InvalidArgument);
    _values.insert_or_assign(std::string { name }, std::string { value });
    return {};
}

std::optional<std::string> PosixProcessEnvironment::get(std::string_view name) const
{
    if (auto i = _values.find(name); i != _values.end())
        return i->second;
    return core::LiveEnvironment {}.get(name);
}

std::expected<void, PlatformError> PosixProcessEnvironment::unset(std::string_view name)
{
    if (!isValidEnvironmentName(name))
        return std::unexpected(PlatformError::InvalidArgument);
    if (auto i = _values.find(name); i != _values.end())
        _values.erase(i);
    return toPlatformResult(core::unsetProcessEnvironmentVariable(name));
}

std::expected<void, PlatformError> PosixProcessEnvironment::exportVariable(std::string_view name)
{
    if (!isValidEnvironmentName(name))
        return std::unexpected(PlatformError::InvalidArgument);
    if (auto i = _values.find(name); i != _values.end())
        return toPlatformResult(core::setProcessEnvironmentVariable(name, i->second));
    return {};
}

std::vector<std::string> PosixProcessEnvironment::keys() const
{
    std::vector<std::string> result;

    // First, collect from system environment
    auto* const* env = processEnviron();
    while (env != nullptr && *env != nullptr)
    {
        std::string_view const entry(*env);
        if (auto const pos = entry.find('='); pos != std::string_view::npos)
            result.emplace_back(entry.substr(0, pos));
        ++env;
    }

    // Add locally-set variables that might not be exported yet
    for (auto const& [key, _]: _values)
    {
        if (std::ranges::find(result, key) == result.end())
            result.push_back(key);
    }

    return result;
}

} // namespace core::platform
