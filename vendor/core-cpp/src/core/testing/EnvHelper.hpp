// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/Environment.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#ifdef _WIN32
    #include <stdlib.h> // _putenv_s
#endif

namespace core::testing
{

/// @brief Cross-platform setenv for tests.
///
/// Prefer injecting a @c core::testing::FakeEnvironment or a
/// @c core::platform::testing::TestProcessEnvironment where the code under test takes one: the
/// process environment is shared by every thread and every test in the binary. This is for code
/// that reads the process environment itself, or passes it on to a child process.
///
/// On Windows it is `_putenv_s()`, which updates both the CRT's copy of the environment that
/// `getenv()` reads and the operating system's block that @c core::LiveEnvironment reads and a child
/// process inherits. An empty @p value is the one case it gets wrong for this purpose: it removes
/// the variable from both, where an empty value is a variable that is *set*, as it is on POSIX and
/// as @c core::LiveEnvironment::get() reports. So the Win32 entry is written again through
/// @c core::setProcessEnvironmentVariable(). The CRT's copy cannot hold an empty value at all --
/// no CRT setter can express one -- so there `getenv()` still reads the variable as absent.
/// Elsewhere this is @c core::setProcessEnvironmentVariable() alone.
///
/// @param name Environment variable name.
/// @param value Environment variable value. An empty value sets the variable.
inline void setTestEnv(char const* name, char const* value)
{
#ifdef _WIN32
    _putenv_s(name, value);
    if (*value == '\0')
        std::ignore = core::setProcessEnvironmentVariable(name, value);
#else
    std::ignore = core::setProcessEnvironmentVariable(name, value);
#endif
}

/// @brief Cross-platform unsetenv for tests.
///
/// On Windows both copies have to be told: `_putenv_s(name, "")` removes the variable from the
/// CRT's copy, and only from the Win32 block as well when the CRT's copy knew about it, which it
/// does not for a variable @ref setTestEnv wrote there with an empty value.
///
/// @param name Environment variable name to unset.
inline void unsetTestEnv(char const* name)
{
#ifdef _WIN32
    _putenv_s(name, "");
    std::ignore = core::unsetProcessEnvironmentVariable(name);
#else
    std::ignore = core::unsetProcessEnvironmentVariable(name);
#endif
}

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
    /// @param name  Environment variable name.
    /// @param value Value to set for the duration of the scope.
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
