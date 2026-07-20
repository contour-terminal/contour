// SPDX-License-Identifier: Apache-2.0
#include <crispy/environment.h>

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <map>
#include <string>
#include <string_view>

#ifdef __APPLE__
    #include <crt_externs.h>
#elifndef _WIN32
extern "C" char** environ;
#endif

namespace crispy::environment
{

namespace
{
#ifdef _WIN32
    [[nodiscard]] constexpr char toLowerASCII(char ch) noexcept
    {
        return 'A' <= ch && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch;
    }
#endif

    /// Orders environment variable names the way the host's own getenv() resolves them: byte-wise on
    /// POSIX, case-insensitively on Windows.
    ///
    /// Windows stores whatever casing the creating process used but matches names case-insensitively,
    /// so a byte-wise map would answer nullopt for a `LOCALAPPDATA` lookup against a block that spells
    /// it `LocalAppData` -- a regression against the getenv() this snapshot replaces.
    struct name_less
    {
        using is_transparent = void;

        [[nodiscard]] bool operator()(std::string_view a, std::string_view b) const noexcept
        {
#ifdef _WIN32
            return std::ranges::lexicographical_compare(
                a, b, std::ranges::less {}, toLowerASCII, toLowerASCII);
#else
            return a < b;
#endif
        }
    };

    using environment_map = std::map<std::string, std::string, name_less>;

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
