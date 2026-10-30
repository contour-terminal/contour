// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/session/Permission.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <variant>

namespace contour::session
{

/// An extension asks to run its declared commands. Which extension, and what it declares, its own consent
/// queue holds; the permission queue only keeps its place in line.
struct ExtensionConsentAsk
{
    constexpr bool operator==(ExtensionConsentAsk const&) const noexcept = default;
};

/// One question a session asks its user.
using PermissionAsk = std::variant<GuardedRole, ExtensionConsentAsk>;

/// Whether the question shown changed.
enum class PromptChange : uint8_t
{
    None = 0, ///< The question shown, if any, is still the one shown.
    Due,      ///< Another question is now the one to show.
};

/// The questions a session asks, one at a time, in the order asked.
///
/// The front is the question shown; the others wait behind it. A question equal to one already waiting is
/// not queued again: each role's pending state already gathers what it is asked about (captures accumulate
/// and one answer answers all; a later font change or paste replaces the earlier), so one answer serves it.
class PermissionQueue
{
  public:
    /// Puts @p ask in line, unless it already waits.
    /// @param ask The question.
    /// @return Due when it became the question to show; None when it waits behind another, or already waited.
    [[nodiscard]] PromptChange offer(PermissionAsk ask)
    {
        if (std::ranges::find(_asks, ask) != _asks.end())
            return PromptChange::None;
        _asks.push_back(ask);
        return _asks.size() == 1 ? PromptChange::Due : PromptChange::None;
    }

    /// @return The question to show now; nothing when nothing is asked.
    [[nodiscard]] std::optional<PermissionAsk> current() const noexcept
    {
        if (_asks.empty())
            return std::nullopt;
        return _asks.front();
    }

    /// Drops the question shown, once it is answered.
    /// @return Due when another question is now the one to show; None when nothing more is asked.
    [[nodiscard]] PromptChange finish()
    {
        if (_asks.empty())
            return PromptChange::None;
        _asks.pop_front();
        return _asks.empty() ? PromptChange::None : PromptChange::Due;
    }

    /// Drops @p ask wherever it waits: it was settled without the user's answer to it (a configured or
    /// remembered answer drained what it asked about).
    /// @param ask The question.
    /// @return Due when it was the question shown and another now is; None otherwise.
    [[nodiscard]] PromptChange withdraw(PermissionAsk const& ask)
    {
        auto const found = std::ranges::find(_asks, ask);
        if (found == _asks.end())
            return PromptChange::None;
        if (found == _asks.begin())
            return finish();
        _asks.erase(found);
        return PromptChange::None;
    }

    /// @return Whether @p ask is asked, shown or waiting.
    [[nodiscard]] bool contains(PermissionAsk const& ask) const
    {
        return std::ranges::find(_asks, ask) != _asks.end();
    }

    /// @return How many questions are asked, the one shown included.
    [[nodiscard]] std::size_t size() const noexcept { return _asks.size(); }

  private:
    std::deque<PermissionAsk> _asks;
};

} // namespace contour::session
