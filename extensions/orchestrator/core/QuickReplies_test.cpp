// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <ranges>
#include <string_view>
#include <utility>

#include <orchestrator/core/QuickReplies.hpp>

using namespace orchestrator;

TEST_CASE("every pane key has one spelling, which reads back as it", "[quick-replies]")
{
    for (auto const [index, name]: std::views::enumerate(paneKeyNames()))
    {
        auto const key = static_cast<PaneKey>(index);
        CHECK(paneKeyName(key) == name);
        CHECK(paneKeyNamed(name) == key);
    }
    CHECK(paneKeyNamed("0") == std::nullopt);
    CHECK(paneKeyNamed("escape") == std::nullopt);
    CHECK(paneKeyNamed("") == std::nullopt);
}

TEST_CASE("what follows a reply reads by its name", "[quick-replies]")
{
    CHECK(afterReplyNamed("wait") == AfterReply::Wait);
    CHECK(afterReplyNamed("message") == AfterReply::Message);
    CHECK(afterReplyNamed("send") == std::nullopt);
    CHECK(afterReplyNames().size() == std::to_underlying(AfterReply::Message) + 1U);
}

TEST_CASE("the default replies are 1, 2 and 3, each on its own digit", "[quick-replies]")
{
    auto const replies = defaultQuickReplies();
    REQUIRE(replies.size() == 3);
    for (auto const [index, reply]: std::views::enumerate(replies))
    {
        CHECK(reply.key == static_cast<char>('1' + index));
        CHECK_FALSE(reply.label.empty());
        CHECK(reply.label.size() <= MaxQuickReplyLabelBytes);
        CHECK(findQuickReply(replies, reply.key) == &replies[static_cast<std::size_t>(index)]);
    }
    CHECK(findQuickReply(replies, '4') == nullptr);
}
