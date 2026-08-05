// SPDX-License-Identifier: Apache-2.0
#include <crispy/environment.h>

#include <optional>
#include <string>
#include <string_view>

#ifdef _WIN32
    #include <vector>

    #include <Windows.h>
#else
    #include <mutex>

    #ifdef __APPLE__
        #include <crt_externs.h>
    #else
extern "C" char** environ;
    #endif
#endif

namespace crispy
{

namespace
{
#ifndef _WIN32
    /// Serializes this translation unit's reads of the environment block against one another.
    ///
    /// Process-wide rather than a member, because what it guards is process-wide: two
    /// live_environment instances read the same block, so a per-instance lock would serialize
    /// nothing.
    [[nodiscard]] std::mutex& environmentMutex() noexcept
    {
        static std::mutex instance;
        return instance;
    }

    /// Looks a name up in the environment block as it stands right now.
    ///
    /// A scan of the block rather than a getenv() call, which does exactly this scan and no better:
    /// glibc's getenv() walks the same array, so a hand-rolled walk is neither slower nor less safe.
    /// It also avoids the thread-unsafe getenv() that this project's clang-tidy configuration
    /// rejects outright.
    ///
    /// @param name Name of the variable to look up.
    /// @return Its value, or std::nullopt if it is not set.
    [[nodiscard]] std::optional<std::string> lookupInEnviron(std::string_view name)
    {
    #ifdef __APPLE__
        auto* const* entry = *_NSGetEnviron();
    #else
        auto* const* entry = environ;
    #endif
        for (; entry != nullptr && *entry != nullptr; ++entry)
        {
            auto const line = std::string_view { *entry };
            if (auto const separator = line.find('=');
                separator != std::string_view::npos && line.substr(0, separator) == name)
                return std::string { line.substr(separator + 1) };
        }
        return std::nullopt;
    }
#endif
} // namespace

std::optional<std::string> live_environment::get(std::string_view name) const
{
#ifdef _WIN32
    // GetEnvironmentVariableA wants a NUL-terminated name, which a string_view does not promise.
    auto const terminatedName = std::string { name };

    // The Win32 block rather than the CRT's copy of it: SetEnvironmentVariable() writes the former
    // and the operating system synchronizes reads of it, whereas the CRT copy is only refreshed by
    // the CRT's own setters.
    auto const required = GetEnvironmentVariableA(terminatedName.c_str(), nullptr, 0);
    if (required == 0)
        return std::nullopt;

    // `required` counts the terminating NUL; the second call's result does not. A writer racing
    // between the two calls can shrink the value, so the second length is the one to trust.
    auto buffer = std::vector<char>(required);
    auto const written = GetEnvironmentVariableA(terminatedName.c_str(), buffer.data(), required);
    if (written == 0 || written >= required)
        return std::nullopt;
    return std::string { buffer.data(), written };
#else
    // The copy has to happen under the lock, not after it: the block holds pointers that a
    // concurrent setenv() may reallocate out from under a reader.
    auto const lock = std::scoped_lock { environmentMutex() };
    return lookupInEnviron(name);
#endif
}

caching_environment::caching_environment(environment const& source) noexcept: _source { source }
{
}

std::optional<std::string> caching_environment::get(std::string_view name) const
{
    // The source is consulted under this lock as well, so two threads racing on the same unseen
    // name read it once rather than twice. The two mutexes are only ever taken in this order.
    auto const lock = std::scoped_lock { _mutex };

    if (auto const i = _cache.find(name); i != _cache.end())
        return i->second;

    return _cache.emplace(name, _source.get(name)).first->second;
}

environment& defaultEnvironment()
{
    static live_environment const source;
    static caching_environment instance { source };
    return instance;
}

} // namespace crispy
