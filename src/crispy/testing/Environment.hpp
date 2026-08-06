// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <crispy/Environment.hpp>

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace crispy::testing
{

/// An environment holding exactly what a test put into it.
///
/// This is the point of @c crispy::Environment being an interface: a test that needs a name to read
/// a certain way constructs one of these and injects it, rather than mutating the environment of
/// the test binary that is running it -- which is thread-unsafe, leaks into every other test in the
/// suite, and has no portable spelling on MSVC.
class FakeEnvironment final: public Environment
{
  public:
    /// Constructs an environment in which every name is unset.
    FakeEnvironment() = default;

    /// @param entries The variables this environment holds; every other name reads as unset.
    explicit FakeEnvironment(std::map<std::string, std::string, std::less<>> entries):
        _entries { std::move(entries) }
    {
    }

    /// @param name Name of the variable to look up.
    /// @return Its value, or std::nullopt if this environment was not given one.
    [[nodiscard]] std::optional<std::string> get(std::string_view name) const override
    {
        if (auto const i = _entries.find(name); i != _entries.end())
            return i->second;
        return std::nullopt;
    }

  private:
    std::map<std::string, std::string, std::less<>> _entries;
};

} // namespace crispy::testing
