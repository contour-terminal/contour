// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/Environment.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace core::testing
{

/// @brief Cross-platform setenv for tests.
///
/// Prefer injecting a @c core::testing::FakeEnvironment or a
/// @c core::platform::testing::TestProcessEnvironment where the code under test takes one: the
/// process environment is shared by every thread and every test in the binary. This is for code
/// that reads the process environment itself, or passes it on to a child process.
///
/// @p name and @p value are UTF-8, as every core-cpp string is. On Windows they are written through
/// the wide CRT, `_wputenv_s()`, converted to UTF-16: it updates both the CRT's copy of the
/// environment (`_wgetenv()`) and the operating system's block that @c core::LiveEnvironment reads
/// and a child process inherits, so each holds the value as written. The narrow `_putenv_s()` read
/// both in the ANSI code page, which spells most of Unicode not at all. The CRT's narrow copy, which
/// `getenv()` reads, still spells the value in that code page, with a `?` for each character it
/// cannot spell. Text that is not UTF-8 has no UTF-16 spelling, so it is not written. An empty
/// @p value is the one case `_wputenv_s()` gets wrong for this purpose: it removes the variable from
/// both, where an empty value is a variable that is *set*, as it is on POSIX and as
/// @c core::LiveEnvironment::get() reports. So the Win32 entry is written again through
/// @c core::setProcessEnvironmentVariable(). The CRT's copy cannot hold an empty value at all -- no
/// CRT setter can express one -- so there `getenv()` still reads the variable as absent. Elsewhere
/// this is @c core::setProcessEnvironmentVariable() alone.
///
/// @param name Environment variable name, in UTF-8.
/// @param value Environment variable value, in UTF-8. An empty value sets the variable.
void setTestEnv(char const* name, char const* value);

/// @brief Cross-platform unsetenv for tests.
///
/// On Windows both copies have to be told: `_wputenv_s(name, L"")` removes the variable from the
/// CRT's copy, and only from the Win32 block as well when the CRT's copy knew about it, which it
/// does not for a variable @ref setTestEnv wrote there with an empty value.
///
/// @param name Environment variable name to unset, in UTF-8.
void unsetTestEnv(char const* name);

/// @brief Sets an environment variable for a scope, restoring the previous value after.
///
/// The environment is process-global, so a test that sets a variable and restores it at the
/// end of the test body leaks that change whenever an assertion throws first. $HOME is the
/// one that bites: other fixtures read it while they set up, so a leaked value silently
/// redirects a later test's history or configuration to the wrong place.
class ScopedEnv
{
  public:
    /// @brief Sets @p name to @p value until this object goes out of scope.
    /// @param name  Environment variable name, in UTF-8.
    /// @param value Value to set for the duration of the scope, in UTF-8.
    ScopedEnv(std::string_view name, std::string_view value):
        _name { name }, _previous { core::LiveEnvironment {}.get(name) }
    {
        setTestEnv(_name.c_str(), std::string { value }.c_str());
    }

    ~ScopedEnv()
    {
        if (_previous)
            setTestEnv(_name.c_str(), _previous->c_str());
        else
            unsetTestEnv(_name.c_str());
    }

    ScopedEnv(ScopedEnv const&) = delete;
    ScopedEnv& operator=(ScopedEnv const&) = delete;
    ScopedEnv(ScopedEnv&&) = delete;
    ScopedEnv& operator=(ScopedEnv&&) = delete;

  private:
    std::string _name;
    std::optional<std::string> _previous;
};

} // namespace core::testing
