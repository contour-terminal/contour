// SPDX-License-Identifier: Apache-2.0
#include <core/UserInfo.hpp>

#include <cerrno>
#include <vector>

#include <pwd.h>
#include <unistd.h>

namespace core
{

namespace
{
    /// @return @p text, or an empty string if @p text is null.
    [[nodiscard]] std::string orEmpty(char const* text)
    {
        return text != nullptr ? std::string { text } : std::string {};
    }
} // namespace

std::optional<PasswordEntry> currentUserPasswordEntry()
{
    // sysconf() only suggests a buffer size; getpwuid_r() answers ERANGE if it was too small.
    auto const suggestedSize = sysconf(_SC_GETPW_R_SIZE_MAX);
    auto buffer = std::vector<char>(suggestedSize > 0 ? static_cast<size_t>(suggestedSize) : 16384);

    while (true)
    {
        auto entry = passwd {};
        passwd* result = nullptr;
        auto const rc = getpwuid_r(getuid(), &entry, buffer.data(), buffer.size(), &result);

        if (rc == ERANGE)
        {
            buffer.resize(buffer.size() * 2);
            continue;
        }

        if (rc != 0 || result == nullptr)
            return std::nullopt;

        return PasswordEntry { .name = orEmpty(result->pw_name),
                               .homeDirectory = orEmpty(result->pw_dir),
                               .shell = orEmpty(result->pw_shell) };
    }
}

} // namespace core
