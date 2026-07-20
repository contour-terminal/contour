// SPDX-License-Identifier: Apache-2.0
#include <crispy/environment.h>

#include <cstdlib>
#include <functional>
#include <map>
#include <string>

#ifdef __APPLE__
    #include <crt_externs.h>
#elifndef _WIN32
extern "C" char** environ;
#endif

namespace crispy::environment
{

namespace
{
    using environment_map = std::map<std::string, std::string, std::less<>>;

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

    /// Copies the process environment into a map, once. Initialization of the function-local
    /// static is thread safe, and the map is never mutated afterwards, so the string_views and C
    /// strings handed out from it stay valid.
    [[nodiscard]] environment_map const& snapshot()
    {
        static auto const entries = []() {
            auto result = environment_map {};
            for (char** entry = currentEnviron(); entry != nullptr && *entry != nullptr; ++entry)
            {
                auto const line = std::string_view { *entry };
                if (auto const separator = line.find('='); separator != std::string_view::npos)
                    result.emplace(line.substr(0, separator), line.substr(separator + 1));
            }
            return result;
        }();
        return entries;
    }
} // namespace

std::optional<std::string_view> get(std::string_view name)
{
    auto const& entries = snapshot();
    if (auto const i = entries.find(name); i != entries.end())
        return std::string_view { i->second };
    return std::nullopt;
}

char const* getCString(std::string_view name)
{
    auto const& entries = snapshot();
    if (auto const i = entries.find(name); i != entries.end())
        return i->second.c_str();
    return nullptr;
}

} // namespace crispy::environment
