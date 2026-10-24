// SPDX-License-Identifier: Apache-2.0
#include <vthost/AttachPlan.hpp>

#include <algorithm>
#include <ranges>

namespace vthost
{

namespace
{
    /// @return The ids of the windows holding at least one tab.
    [[nodiscard]] std::vector<uint64_t> windowsWithTabs(AttachContext const& context)
    {
        return context.windows
               | std::views::filter([](WindowSummary const& window) { return window.tabCount > 0; })
               | std::views::transform(&WindowSummary::id) | std::ranges::to<std::vector>();
    }

    /// @return The ids in @p wanted that name an existing window, ascending.
    [[nodiscard]] std::vector<uint64_t> existing(std::vector<uint64_t> const& wanted,
                                                 AttachContext const& context)
    {
        return context.windows | std::views::transform(&WindowSummary::id)
               | std::views::filter([&](uint64_t id) { return std::ranges::contains(wanted, id); })
               | std::ranges::to<std::vector>();
    }

    /// Every window with tabs; with none, the primary window gets its first tab.
    [[nodiscard]] AttachPlan takeEverything(FollowScope scope, AttachContext const& context)
    {
        auto follow = windowsWithTabs(context);
        if (!follow.empty())
            return { .scope = scope, .follow = std::move(follow), .creation = AttachCreation::None };
        return { .scope = scope,
                 .follow = { context.primaryWindow },
                 .creation = AttachCreation::FirstTabInPrimary };
    }

    [[nodiscard]] AttachPlan newWindow()
    {
        return { .scope = FollowScope::Listed, .follow = {}, .creation = AttachCreation::NewWindow };
    }

    /// The first client of a daemon takes what is there; a later one gets a window of its own.
    [[nodiscard]] AttachPlan launch(AttachContext const& context)
    {
        return context.otherClients == 0 ? takeEverything(FollowScope::Listed, context) : newWindow();
    }
} // namespace

AttachPlan planAttach(proto::AttachIntent const& intent, AttachContext const& context)
{
    switch (intent.kind)
    {
        case proto::AttachIntentKind::AllWindows: return takeEverything(FollowScope::Every, context);
        case proto::AttachIntentKind::Launch: return launch(context);
        case proto::AttachIntentKind::NewWindow: return newWindow();
        case proto::AttachIntentKind::Windows:
            return { .scope = FollowScope::Listed,
                     .follow = existing(intent.windows, context),
                     .creation = AttachCreation::None };
        case proto::AttachIntentKind::Resume: {
            // Review focus 2: the windows are gone (a restarted daemon), so this is a fresh launch. A window
            // holding no tab is no survivor: it is a restarted daemon's own empty primary window, whose id is
            // the lost one's only because every run counts from 1.
            auto follow =
                windowsWithTabs(context)
                | std::views::filter([&](uint64_t id) { return std::ranges::contains(intent.windows, id); })
                | std::ranges::to<std::vector>();
            if (follow.empty())
                return launch(context);
            return { .scope = FollowScope::Listed,
                     .follow = std::move(follow),
                     .creation = AttachCreation::None };
        }
    }
    return newWindow();
}

} // namespace vthost
