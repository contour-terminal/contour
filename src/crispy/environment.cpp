// SPDX-License-Identifier: Apache-2.0
#include <crispy/environment.h>

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <string>
#include <string_view>

#ifdef _WIN32
    #include <vector>

    #include <Windows.h>
#else
    #include <mutex>
#endif

#ifdef __APPLE__
    #include <crt_externs.h>
#elifndef _WIN32
extern "C" char** environ;
#endif

namespace crispy
{

namespace
{
#ifdef _WIN32
    [[nodiscard]] constexpr char toLowerASCII(char ch) noexcept
    {
        return 'A' <= ch && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch;
    }
#else
    /// Serializes this translation unit's getenv() calls against one another.
    ///
    /// Process-wide rather than a member, because what it guards is process-wide: two
    /// live_environment instances read the same block, so a per-instance lock would serialize
    /// nothing.
    [[nodiscard]] std::mutex& environmentMutex() noexcept
    {
        static std::mutex instance;
        return instance;
    }
#endif

    [[nodiscard]] char** currentEnviron() noexcept
    {
#ifdef __APPLE__
        return *_NSGetEnviron();
#elifdef _WIN32
        return _environ;
#else
        return environ;
#endif
    }
} // namespace

bool snapshot_environment::name_less::operator()(std::string_view a, std::string_view b) const noexcept
{
#ifdef _WIN32
    return std::ranges::lexicographical_compare(a, b, std::ranges::less {}, toLowerASCII, toLowerASCII);
#else
    return a < b;
#endif
}

snapshot_environment::snapshot_environment()
{
    for (char** entry = currentEnviron(); entry != nullptr && *entry != nullptr; ++entry)
    {
        auto const line = std::string_view { *entry };
        if (auto const separator = line.find('='); separator != std::string_view::npos)
            _entries.emplace(line.substr(0, separator), line.substr(separator + 1));
    }
}

std::optional<std::string> snapshot_environment::get(std::string_view name) const
{
    if (auto const i = _entries.find(name); i != _entries.end())
        return i->second;
    return std::nullopt;
}

std::optional<std::string> live_environment::get(std::string_view name) const
{
    // Both platform APIs below want a NUL-terminated name, which a string_view does not promise.
    auto const terminatedName = std::string { name };

#ifdef _WIN32
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
    // The copy has to happen under the lock, not after it: getenv() hands back a pointer into a
    // block that a concurrent setenv() may reallocate.
    auto const lock = std::scoped_lock { environmentMutex() };
    if (char const* value = std::getenv(terminatedName.c_str()))
        return std::string { value };
    return std::nullopt;
#endif
}

environment& defaultEnvironment()
{
    static snapshot_environment instance;
    return instance;
}

environment& defaultLiveEnvironment() noexcept
{
    static live_environment instance;
    return instance;
}

} // namespace crispy
