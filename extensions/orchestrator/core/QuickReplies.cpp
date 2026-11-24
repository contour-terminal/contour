// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/core/QuickReplies.hpp>

#include <algorithm>
#include <array>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

using namespace std::string_view_literals;

namespace orchestrator
{

namespace
{
    /// Indexed by PaneKey.
    constexpr auto PaneKeyNames = std::array {
        "1"sv, "2"sv, "3"sv, "4"sv, "5"sv, "6"sv, "7"sv, "8"sv, "9"sv, "esc"sv,
    };
    static_assert(PaneKeyNames.size() == std::to_underlying(PaneKey::Escape) + 1U);

    /// Indexed by AfterReply.
    constexpr auto AfterReplyNames = std::array { "wait"sv, "message"sv };
    static_assert(AfterReplyNames.size() == std::to_underlying(AfterReply::Message) + 1U);

    /// @return The index of @p text in @p names; std::nullopt when it is none of them.
    template <std::size_t N>
    [[nodiscard]] std::optional<std::size_t> indexOf(std::array<std::string_view, N> const& names,
                                                     std::string_view text) noexcept
    {
        auto const found = std::ranges::find(names, text);
        return found != names.end() ? std::optional { static_cast<std::size_t>(found - names.begin()) }
                                    : std::nullopt;
    }
} // namespace

std::vector<QuickReply> defaultQuickReplies()
{
    return {
        QuickReply { .key = '1', .label = "yes", .press = PaneKey::Digit1, .then = AfterReply::Wait },
        QuickReply {
            .key = '2', .label = "yes, don't ask again", .press = PaneKey::Digit2, .then = AfterReply::Wait },
        QuickReply {
            .key = '3', .label = "no, and say why", .press = PaneKey::Escape, .then = AfterReply::Message },
    };
}

std::optional<PaneKey> paneKeyNamed(std::string_view text) noexcept
{
    return indexOf(PaneKeyNames, text).transform([](std::size_t index) {
        return static_cast<PaneKey>(index);
    });
}

std::string_view paneKeyName(PaneKey key) noexcept
{
    auto const index = std::to_underlying(key);
    return index < PaneKeyNames.size() ? PaneKeyNames[index] : std::string_view {};
}

std::span<std::string_view const> paneKeyNames() noexcept
{
    return PaneKeyNames;
}

std::optional<AfterReply> afterReplyNamed(std::string_view text) noexcept
{
    return indexOf(AfterReplyNames, text).transform([](std::size_t index) {
        return static_cast<AfterReply>(index);
    });
}

std::span<std::string_view const> afterReplyNames() noexcept
{
    return AfterReplyNames;
}

QuickReply const* findQuickReply(std::span<QuickReply const> replies, char key) noexcept
{
    auto const found = std::ranges::find(replies, key, &QuickReply::key);
    return found != replies.end() ? &*found : nullptr;
}

} // namespace orchestrator
