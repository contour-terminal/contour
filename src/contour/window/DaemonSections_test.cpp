// SPDX-License-Identifier: Apache-2.0
#include <contour/test/FakeDaemonSections.hpp>
#include <contour/window/DaemonSections.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <utility>
#include <vector>

using contour::test::DaemonSectionsCall;
using contour::test::FakeDaemonSections;
using contour::window::DaemonInstance;
using contour::window::DaemonSection;
using contour::window::hostDaemonRef;
using contour::window::localDaemonRef;
using contour::window::PaneKey;
using contour::window::PaneSeen;
using contour::window::SectionsSubscribers;
using contour::window::SectionsSubscription;
using contour::window::SidebarListingChoice;
using contour::window::TabRef;

TEST_CASE("a subscriber another one's call unsubscribed is not told", "[daemon-sections]")
{
    auto subscribers = SectionsSubscribers {};
    auto told = std::vector<int> {};
    auto second = SectionsSubscription {};
    auto third = SectionsSubscription {};
    auto const first = subscribers.subscribe([&] {
        told.push_back(1);
        second = SectionsSubscription {};
        third = subscribers.subscribe([&] { told.push_back(3); });
    });
    second = subscribers.subscribe([&] { told.push_back(2); });

    subscribers.notify();
    CHECK(told == std::vector { 1 });
    CHECK(subscribers.size() == 2);
}

TEST_CASE("destroying a subscription unsubscribes", "[daemon-sections]")
{
    auto ended = 0;
    {
        auto const subscription = SectionsSubscription { [&] { ++ended; } };
        CHECK(ended == 0);
    }
    CHECK(ended == 1);

    SECTION("assigning over a subscription ends the one it held")
    {
        auto replaced = 0;
        auto subscription = SectionsSubscription { [&] { ++ended; } };
        subscription = SectionsSubscription { [&] { ++replaced; } };
        CHECK(ended == 2);
        CHECK(replaced == 0);
        subscription = SectionsSubscription {};
        CHECK(replaced == 1);
    }
}

TEST_CASE("a moved-from subscription unsubscribes nothing", "[daemon-sections]")
{
    auto ended = 0;
    {
        auto first = SectionsSubscription { [&] { ++ended; } };
        {
            auto const second = SectionsSubscription { std::move(first) };
            CHECK(ended == 0);
        }
        CHECK(ended == 1);

        auto third = SectionsSubscription { [&] { ++ended; } };
        auto fourth = SectionsSubscription {};
        fourth = std::move(third);
        CHECK(ended == 1);
    }
    // first and third were moved from: only fourth ends something as the scope closes.
    CHECK(ended == 2);
}

TEST_CASE("the fake records verbs and tells its subscribers", "[daemon-sections]")
{
    auto fake = FakeDaemonSections {};
    auto section = DaemonSection {};
    section.daemon = localDaemonRef("work");
    fake.listed = { section };
    CHECK(fake.sections() == std::vector { section });

    auto const work = localDaemonRef("work");
    auto const run = DaemonInstance { 9 };
    auto const tab = TabRef { .workspace = 2, .tab = 3 };
    auto const seen =
        PaneSeen { .serials = { .activity = 1, .bell = 2, .finished = 3 }, .statusGeneration = 4 };
    fake.connect(hostDaemonRef("starfive"), vtworkspace::WorkspaceId(5));
    fake.start(work);
    fake.remove(work);
    fake.setListed(work, SidebarListingChoice::Listed);
    fake.renameWorkspace(work, run, 2, "build");
    fake.renameTab(work, run, tab, "logs");
    fake.setWorkspaceColor(work, run, 2, std::nullopt);
    fake.closeTab(work, run, tab);
    fake.closeWorkspace(work, run, 2);
    fake.markSeen(work, run, PaneKey { 11 }, seen);
    using Call = DaemonSectionsCall;
    CHECK(fake.calls
          == std::vector<Call> {
              { Call::Connect { .daemon = hostDaemonRef("starfive"),
                                .askingWindow = vtworkspace::WorkspaceId(5) } },
              { Call::Start { .daemon = work } },
              { Call::Remove { .daemon = work } },
              { Call::SetListed { .daemon = work, .choice = SidebarListingChoice::Listed } },
              { Call::RenameWorkspace { .daemon = work, .instance = run, .workspace = 2, .name = "build" } },
              { Call::RenameTab { .daemon = work, .instance = run, .tab = tab, .name = "logs" } },
              { Call::SetWorkspaceColor {
                  .daemon = work, .instance = run, .workspace = 2, .color = std::nullopt } },
              { Call::CloseTab { .daemon = work, .instance = run, .tab = tab } },
              { Call::CloseWorkspace { .daemon = work, .instance = run, .workspace = 2 } },
              { Call::MarkSeen { .daemon = work, .instance = run, .pane = PaneKey { 11 }, .seen = seen } },
          });

    auto told = 0;
    auto subscription = fake.onChanged([&] { ++told; });
    auto const other = fake.onChanged([&] { told += 10; });
    CHECK(fake.subscriberCount() == 2);
    fake.changed();
    CHECK(told == 11);

    subscription = SectionsSubscription {};
    CHECK(fake.subscriberCount() == 1);
    fake.changed();
    CHECK(told == 21);
}
