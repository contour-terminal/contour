// SPDX-License-Identifier: Apache-2.0
#include <contour/session/LocalFileAction.hpp>

#include <core/Utf8Path.hpp>

#include <algorithm>
#include <string>

namespace contour::session
{

namespace
{
    /// @return @p c in ASCII lower case, whatever the C locale: std::tolower() under a Turkish single-byte
    ///         locale does not map 'I' to 'i'.
    [[nodiscard]] constexpr char asciiLower(char c) noexcept
    {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    }

    /// @return Whether @p name ends in @p suffix, compared without regard to ASCII case.
    [[nodiscard]] constexpr bool endsWithIgnoringAsciiCase(std::string_view name,
                                                           std::string_view suffix) noexcept
    {
        return name.size() >= suffix.size()
               && std::ranges::equal(
                   name.substr(name.size() - suffix.size()), suffix, {}, asciiLower, asciiLower);
    }

    static_assert(endsWithIgnoringAsciiCase("BUILD.CMD", ".cmd"));
    static_assert(endsWithIgnoringAsciiCase("setup.Exe", ".exe"));
    static_assert(!endsWithIgnoringAsciiCase("exe", ".exe"));

    /// @return Whether the regular file @p path is a program by @p rule.
    [[nodiscard]] bool isProgram(std::filesystem::path const& path,
                                 core::platform::FileSystem const& fileSystem,
                                 ExecutableRule rule)
    {
        switch (rule)
        {
            case ExecutableRule::ExecuteBit: return fileSystem.isExecutableFile(path);
            case ExecutableRule::Suffix: {
                auto const name = core::pathToUtf8(path.filename());
                return std::ranges::any_of(WindowsExecutableSuffixes, [&](std::string_view suffix) {
                    return endsWithIgnoringAsciiCase(name, suffix);
                });
            }
        }
        return false;
    }
} // namespace

LocalFileAction localFileAction(std::filesystem::path const& path,
                                core::platform::FileSystem const& fileSystem,
                                ExecutableRule rule,
                                std::string_view editor)
{
    if (!fileSystem.isRegularFile(path))
        return LocalFileAction::OpenOnDesktop;
    if (isProgram(path, fileSystem, rule))
        return LocalFileAction::Run;
    return editor.empty() ? LocalFileAction::OpenOnDesktop : LocalFileAction::Edit;
}

} // namespace contour::session
