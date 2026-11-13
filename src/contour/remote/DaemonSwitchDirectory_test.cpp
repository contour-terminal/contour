// SPDX-License-Identifier: Apache-2.0
#include <contour/remote/DaemonSwitchDirectory.hpp>

#include <QtCore/QCoreApplication>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <format>
#include <latch>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using contour::remote::DaemonLinkId;
using contour::remote::DaemonSwitchDirectory;
using contour::remote::LinkedDaemon;
using contour::remote::ListTarget;
using contour::remote::WindowOnDaemon;
using contour::remote::WorkspacesListed;
using vthost::proto::SwitchDaemonKind;
using vtworkspace::WorkspaceId;

namespace
{

/// Polls @p predicate, running the GUI thread's queued work in between, for up to two seconds.
/// @return Whether it came to hold.
[[nodiscard]] bool waitUntil(auto const& predicate)
{
    for ([[maybe_unused]] auto const attempt: std::views::iota(0, 400))
    {
        QCoreApplication::processEvents();
        if (predicate())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds { 5 });
    }
    return predicate();
}

/// A discovery that answers what the test set, once the test lets it, and counts how often it was asked.
class FakeDiscovery final: public contour::cli::IDaemonDiscovery
{
  public:
    explicit FakeDiscovery(std::vector<contour::cli::DiscoveredDaemon> found): _found(std::move(found)) {}

    std::vector<contour::cli::DiscoveredDaemon> discover() override
    {
        ++asked;
        released.wait();
        return _found;
    }

    std::atomic<int> asked = 0;
    std::latch released { 1 }; ///< Counted down by the test to let discover() answer.

  private:
    std::vector<contour::cli::DiscoveredDaemon> _found;
};

/// A lister that holds each listing until the test answers it.
class FakeLister final: public contour::remote::IWorkspaceLister
{
  public:
    struct Asked
    {
        ListTarget target;
        WorkspacesListed done;
    };

    void list(ListTarget target, WorkspacesListed done) override
    {
        asked.push_back(Asked { .target = std::move(target), .done = std::move(done) });
    }

    /// Answers the listing of @p daemon asked first and not answered yet.
    void answer(std::string const& daemon, contour::remote::WorkspaceListing listed)
    {
        auto const found = std::ranges::find_if(
            asked, [&](Asked const& entry) { return entry.target.address.name == daemon; });
        REQUIRE(found != asked.end());
        auto done = std::move(found->done);
        asked.erase(found);
        done(std::move(listed));
    }

    std::vector<Asked> asked;
};

[[nodiscard]] vtcontrol::WorkspaceInfo workspace(uint64_t native,
                                                 uint32_t position,
                                                 std::string label,
                                                 std::string visibility)
{
    auto const clients = visibility == "shown" ? 1U : 0U;
    return vtcontrol::WorkspaceInfo {
        .id = std::format("workspace:00000000-0000-4000-8000-{:012}", native),
        .label = std::move(label),
        .position = position,
        .native = std::to_string(native),
        .visibility = std::move(visibility),
        .clients = clients,
        // One tab: a workspace holding none is nothing to switch to, and is not listed.
        .tabs = { vtcontrol::TabInfo {} },
    };
}

/// The directory over a fake discovery, a fake lister and the linked daemons a test sets.
struct DirectoryRig
{
    std::shared_ptr<FakeDiscovery> discovery =
        std::make_shared<FakeDiscovery>(std::vector<contour::cli::DiscoveredDaemon> {
            { .label = "default", .socket = {}, .workspaces = 2 },
            { .label = "admin", .socket = {}, .workspaces = std::nullopt },
        });
    FakeLister* lister = nullptr;
    std::vector<LinkedDaemon> linked {
        LinkedDaemon { .link = DaemonLinkId { 1 },
                       .address = { .kind = SwitchDaemonKind::Label, .name = "admin" },
                       .transport = nullptr,
                       .windows = { WindowOnDaemon { .window = WorkspaceId { 10 }, .workspace = 7 } } },
        LinkedDaemon { .link = DaemonLinkId { 2 },
                       .address = { .kind = SwitchDaemonKind::Host, .name = "studio" },
                       .transport = nullptr,
                       .windows = { WindowOnDaemon { .window = WorkspaceId { 11 }, .workspace = 3 } } },
        LinkedDaemon { .link = DaemonLinkId { 3 },
                       .address = { .kind = SwitchDaemonKind::Host, .name = "studio" },
                       .transport = nullptr,
                       .windows = { WindowOnDaemon { .window = WorkspaceId { 12 }, .workspace = 4 } } },
        // A link a switch started, serving no window yet: its host is not shown here.
        LinkedDaemon { .link = DaemonLinkId { 4 },
                       .address = { .kind = SwitchDaemonKind::Host, .name = "attic" },
                       .transport = nullptr,
                       .windows = {} },
    };
    std::unique_ptr<DaemonSwitchDirectory> directory;

    DirectoryRig()
    {
        auto made = std::make_unique<FakeLister>();
        lister = made.get();
        directory =
            std::make_unique<DaemonSwitchDirectory>(discovery, std::move(made), [this] { return linked; });
    }

    DirectoryRig(DirectoryRig const&) = delete;
    DirectoryRig& operator=(DirectoryRig const&) = delete;
    DirectoryRig(DirectoryRig&&) = delete;
    DirectoryRig& operator=(DirectoryRig&&) = delete;

    ~DirectoryRig()
    {
        // A discovery still held must answer for the directory's worker to be joined.
        if (discovery->released.try_wait())
            return;
        discovery->released.count_down();
    }
};

} // namespace

TEST_CASE("DaemonSwitchDirectory: local daemons come from a discovery that never blocks the GUI thread",
          "[switch][directory]")
{
    auto rig = DirectoryRig {};
    CHECK(rig.directory->localDaemons().empty()); // nothing discovered before a refresh

    rig.directory->refresh();
    // The discovery runs, and holds, on its own thread: the GUI thread is back here, answering the cache.
    REQUIRE(waitUntil([&] { return rig.discovery->asked == 1; }));
    CHECK(rig.directory->localDaemons().empty());
    rig.directory->refresh(); // one under way: no second
    rig.discovery->released.count_down();

    REQUIRE(waitUntil([&] { return rig.directory->localDaemons().size() == 2; }));
    CHECK(rig.directory->localDaemons().front().label == "default");
    CHECK(rig.discovery->asked == 1);

    rig.directory->refresh(); // once answered, a refresh discovers again
    REQUIRE(waitUntil([&] { return rig.discovery->asked == 2; }));
}

TEST_CASE("DaemonSwitchDirectory: the hosts shown here are the saved hosts a window is on, once each",
          "[switch][directory]")
{
    auto rig = DirectoryRig {};
    CHECK(rig.directory->hostsShownHere() == std::vector<std::string> { "studio" });

    rig.linked.erase(rig.linked.begin() + 1, rig.linked.begin() + 3);
    CHECK(rig.directory->hostsShownHere().empty());
}

TEST_CASE("DaemonSwitchDirectory: a window's workspaces are its daemon's every one, hidden and left too",
          "[switch][directory]")
{
    auto rig = DirectoryRig {};
    CHECK(rig.directory->workspacesOf(WorkspaceId { 10 }).empty()); // nothing listed before a refresh

    rig.directory->refresh();
    // One listing per daemon, not per link: both studio windows share theirs.
    REQUIRE(rig.lister->asked.size() == 3);
    rig.directory->refresh(); // under way: not asked again
    CHECK(rig.lister->asked.size() == 3);

    rig.lister->answer("admin",
                       std::vector {
                           workspace(9, 3, "", "left"),
                           workspace(7, 1, "ops", "shown"),
                           workspace(8, 2, "build", "hidden"),
                       });
    auto const known = rig.directory->workspacesOf(WorkspaceId { 10 });
    REQUIRE(known.size() == 3);
    CHECK(known[0].position == 1);
    CHECK(known[0].name == "ops");
    CHECK(known[0].role == contour::session::WorkspaceRole::Current);
    CHECK(known[1].name == "build");
    CHECK(known[1].role == contour::session::WorkspaceRole::Other);
    CHECK(known[2].name == "workspace 3");
    CHECK(known[2].role == contour::session::WorkspaceRole::Other);

    SECTION("the current one follows what the window shows now, without a new listing")
    {
        rig.linked[0].windows[0].workspace = 8;
        auto const now = rig.directory->workspacesOf(WorkspaceId { 10 });
        REQUIRE(now.size() == 3);
        CHECK(now[0].role == contour::session::WorkspaceRole::Other);
        CHECK(now[1].role == contour::session::WorkspaceRole::Current);
    }

    SECTION("a failed listing keeps the last answer")
    {
        rig.directory->refresh();
        rig.lister->answer("admin",
                           std::unexpected(contour::remote::ListFailure { .reason = "daemon went away" }));
        CHECK(rig.directory->workspacesOf(WorkspaceId { 10 }).size() == 3);
    }

    SECTION("a daemon no window is on any more is forgotten")
    {
        auto const admin = rig.linked.front();
        rig.linked.erase(rig.linked.begin());
        rig.directory->refresh();
        rig.linked.insert(rig.linked.begin(), admin);
        CHECK(rig.directory->workspacesOf(WorkspaceId { 10 }).empty());
    }

    SECTION("a window no daemon here serves has none")
    {
        CHECK(rig.directory->workspacesOf(WorkspaceId { 99 }).empty());
    }
}
