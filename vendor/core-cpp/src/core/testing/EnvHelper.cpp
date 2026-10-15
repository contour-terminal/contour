// SPDX-License-Identifier: Apache-2.0
#include <core/testing/EnvHelper.hpp>

#include <core/Environment.hpp>

#include <tuple>

#ifdef _WIN32
    #include <core/WideString.hpp>

    #include <cstdlib> // _wputenv_s
#endif

namespace core::testing
{

void setTestEnv(char const* name, char const* value)
{
#ifdef _WIN32
    // In UTF-16, as the block is: the narrow _putenv_s() read the name and value in the ANSI code
    // page, so a value beyond it reached neither copy as written.
    auto const wideName = core::toUtf16(name);
    auto const wideValue = core::toUtf16(value);
    if (wideName && wideValue)
        _wputenv_s(wideName->c_str(), wideValue->c_str());
    if (*value == '\0')
        std::ignore = core::setProcessEnvironmentVariable(name, value);
#else
    std::ignore = core::setProcessEnvironmentVariable(name, value);
#endif
}

void unsetTestEnv(char const* name)
{
#ifdef _WIN32
    if (auto const wideName = core::toUtf16(name))
        _wputenv_s(wideName->c_str(), L"");
#endif
    std::ignore = core::unsetProcessEnvironmentVariable(name);
}

} // namespace core::testing
