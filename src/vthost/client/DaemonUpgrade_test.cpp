// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <memory>
#include <optional>
#include <ranges>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vthost/DaemonBinary.hpp>
#include <vthost/client/ClientSupervisor.hpp>
#include <vthost/client/DaemonUpgrade.hpp>
#include <vthost/handover/Readiness.hpp>
#include <vthost/proto/Negotiation.hpp>

namespace
{
using vthost::ImageOrigin;
using vthost::client::ControlError;
using vthost::client::ControlErrorCode;
using vthost::client::DaemonUpgradeError;
using vthost::handover::UpgradeRefusal;
namespace proto = vthost::proto;

/// A lease that says whether it is still held.
class WatchedLease final: public vthost::ImageLease
{
  public:
    explicit WatchedLease(std::shared_ptr<bool> held): _held(std::move(held)) { *_held = true; }
    WatchedLease(WatchedLease const&) = delete;
    WatchedLease& operator=(WatchedLease const&) = delete;
    WatchedLease(WatchedLease&&) = delete;
    WatchedLease& operator=(WatchedLease&&) = delete;
    ~WatchedLease() override { *_held = false; }

  private:
    std::shared_ptr<bool> _held;
};

/// A daemon that answers its status and every upgrade as told, and records what it was asked.
class FakeControlPort final: public vthost::client::DaemonControlPort
{
  public:
    [[nodiscard]] std::expected<vthost::client::DaemonStatus, ControlError> status() override
    {
        ++statusRequests;
        if (!upgradeAsked && statusesToFail > 0)
        {
            --statusesToFail;
            return std::unexpected(ControlError { .code = ControlErrorCode::Transport, .detail = "broken" });
        }
        if (upgradeAsked && !laterStatuses.empty())
        {
            auto const next = laterStatuses.front();
            laterStatuses.pop_front();
            if (!next)
                return std::unexpected(ControlError { .code = ControlErrorCode::NotRunning, .detail = {} });
            statusRelease = *next;
            if (!laterRecords.empty())
            {
                statusRecord = laterRecords.front();
                laterRecords.pop_front();
            }
        }
        if (statusError)
            return std::unexpected(*statusError);
        auto status = vthost::client::DaemonStatus {};
        status.releaseVersion = statusRelease;
        status.capabilities = offered;
        status.upgrade = statusRecord;
        return status;
    }

    [[nodiscard]] std::expected<void, ControlError> stop(proto::StopReason) override { return {}; }

    [[nodiscard]] std::expected<proto::UpgradeDaemonReply, ControlError> upgrade(std::string imageId) override
    {
        upgrades.push_back(std::move(imageId));
        upgradeAsked = true;
        if (releaseAfterAsked)
            statusRelease = *releaseAfterAsked;
        leaseHeldWhileAsked = leaseHeld && *leaseHeld;
        if (upgradeError)
            return std::unexpected(*upgradeError);
        return reply;
    }

    proto::CapabilitySet offered = proto::CapabilitySet::of(
        { proto::Capability::DaemonControl, proto::Capability::LiveUpgrade }); ///< What it offers.
    std::optional<ControlError> statusError;                                   ///< What status() fails with.
    std::optional<ControlError> upgradeError;                                  ///< What upgrade() fails with.
    proto::UpgradeDaemonReply reply {};                                        ///< What upgrade() answers.
    std::string statusRelease = "1.0.0"; ///< The release status() reports.
    bool upgradeAsked = false;           ///< Whether upgrade() ran.
    std::size_t statusesToFail = 0;      ///< How many status() calls fail before upgrade() ran.
    /// What status() answers, one per call, once upgrade() ran: a release, or nullopt for no daemon.
    std::deque<std::optional<std::string>> laterStatuses;
    std::optional<std::string> releaseAfterAsked; ///< The release status() reports once upgrade() ran.
    /// The upgrade record status() reports; nullopt as a daemon from before the record.
    std::optional<proto::DaemonUpgradeRecord> statusRecord;
    /// The record each of laterStatuses' daemons reports, one per daemon that answers, while any are left.
    std::deque<std::optional<proto::DaemonUpgradeRecord>> laterRecords;
    std::shared_ptr<bool> leaseHeld;   ///< The staged image's lease, as the source handed it out.
    std::vector<std::string> upgrades; ///< Every image asked for, in order.
    std::size_t statusRequests = 0;    ///< How often status() was asked.
    bool leaseHeldWhileAsked = false;  ///< Whether the lease was held at the last upgrade().
};

/// A source that hands out one image of the given origin, with a watched lease when staged.
class FakeBinarySource final: public vthost::DaemonBinarySource
{
  public:
    FakeBinarySource(ImageOrigin origin, std::string imageId): _origin(origin), _imageId(std::move(imageId))
    {
    }

    [[nodiscard]] vthost::StagedImage imageForSpawn() override
    {
        ++calls;
        auto lease = std::unique_ptr<vthost::ImageLease> {};
        if (_origin == ImageOrigin::Staged)
            lease = std::make_unique<WatchedLease>(leaseHeld);
        return vthost::StagedImage { .executable = "/state/bin/" + _imageId + "/contour",
                                     .lease = std::move(lease),
                                     .origin = _origin,
                                     .imageId = _imageId };
    }

    std::shared_ptr<bool> leaseHeld = std::make_shared<bool>(false); ///< Whether the image is leased now.
    std::size_t calls = 0;                                           ///< How often it was asked.

  private:
    ImageOrigin _origin;
    std::string _imageId;
};

/// A port and a source of a staged image, wired so the port sees the lease.
struct Upgrade
{
    Upgrade(): Upgrade(ImageOrigin::Staged, "1.2.3-0123abcd") {}
    Upgrade(ImageOrigin origin, std::string imageId): binary(origin, std::move(imageId))
    {
        control.leaseHeld = binary.leaseHeld;
    }

    FakeControlPort control;
    FakeBinarySource binary;
};
} // namespace

TEST_CASE("an upgrade request outwaits the daemon's readiness probe", "[vthost][client][upgrade]")
{
    // The daemon answers only once the probe did, and captured every session after it: a client that gave up
    // first would report a failure for an upgrade that goes on.
    using namespace std::chrono_literals;
    STATIC_REQUIRE(vthost::client::DefaultUpgradeTimeout >= vthost::handover::DefaultReadinessTimeout + 10s);
}

TEST_CASE("upgradeDaemon sends the staged image id", "[vthost][client][upgrade]")
{
    auto h = Upgrade {};
    REQUIRE(vthost::client::upgradeDaemon(h.control, h.binary).has_value());
    CHECK(h.control.upgrades == std::vector<std::string> { "1.2.3-0123abcd" });
    // Held until the daemon answered, released after.
    CHECK(h.control.leaseHeldWhileAsked);
    CHECK_FALSE(*h.binary.leaseHeld);
}

TEST_CASE("upgradeDaemon of a client running in place names the daemon's installed executable",
          "[vthost][client][upgrade]")
{
    auto h = Upgrade { ImageOrigin::Installed, "" };
    REQUIRE(vthost::client::upgradeDaemon(h.control, h.binary).has_value());
    CHECK(h.control.upgrades == std::vector<std::string> { "" });
}

TEST_CASE("upgradeDaemon does not ask the daemon when staging failed", "[vthost][client][upgrade]")
{
    auto h = Upgrade { ImageOrigin::StagingFailed, "" };
    auto const upgraded = vthost::client::upgradeDaemon(h.control, h.binary);
    REQUIRE_FALSE(upgraded.has_value());
    CHECK(upgraded.error().error == DaemonUpgradeError::StageFailed);
    CHECK_FALSE(upgraded.error().refusal.has_value());
    CHECK(h.control.upgrades.empty());
}

TEST_CASE("upgradeDaemon stages nothing without a daemon to upgrade", "[vthost][client][upgrade]")
{
    SECTION("no daemon runs")
    {
        auto h = Upgrade {};
        h.control.statusError = ControlError { .code = ControlErrorCode::NotRunning, .detail = {} };
        auto const upgraded = vthost::client::upgradeDaemon(h.control, h.binary);
        REQUIRE_FALSE(upgraded.has_value());
        CHECK(upgraded.error().error == DaemonUpgradeError::NotRunning);
        CHECK(h.binary.calls == 0);
        CHECK(h.control.upgrades.empty());
    }
    SECTION("the daemon offers no live upgrade")
    {
        auto h = Upgrade {};
        h.control.offered = proto::CapabilitySet::of({ proto::Capability::DaemonControl });
        auto const upgraded = vthost::client::upgradeDaemon(h.control, h.binary);
        REQUIRE_FALSE(upgraded.has_value());
        CHECK(upgraded.error().error == DaemonUpgradeError::Refused);
        CHECK_FALSE(upgraded.error().refusal.has_value());
        CHECK(upgraded.error().detail.contains("1.0.0"));
        CHECK(h.binary.calls == 0);
        CHECK(h.control.upgrades.empty());
    }
}

TEST_CASE("upgradeDaemon carries the daemon's refusal", "[vthost][client][upgrade]")
{
    SECTION("a refusal this build knows")
    {
        auto h = Upgrade {};
        h.control.reply = proto::UpgradeDaemonReply { .result = static_cast<uint8_t>(
                                                          1 + std::to_underlying(UpgradeRefusal::Sandboxed)),
                                                      .detail = "in a sandbox" };
        auto const upgraded = vthost::client::upgradeDaemon(h.control, h.binary);
        REQUIRE_FALSE(upgraded.has_value());
        CHECK(upgraded.error().error == DaemonUpgradeError::Refused);
        CHECK(upgraded.error().refusal == UpgradeRefusal::Sandboxed);
        CHECK(upgraded.error().detail == "in a sandbox");
    }
    SECTION("a refusal without words reads as the refusal's own")
    {
        auto h = Upgrade {};
        h.control.reply = proto::UpgradeDaemonReply { .result = 1, .detail = {} };
        auto const upgraded = vthost::client::upgradeDaemon(h.control, h.binary);
        REQUIRE_FALSE(upgraded.has_value());
        CHECK(upgraded.error().refusal == UpgradeRefusal::InProgress);
        CHECK(upgraded.error().detail == vthost::handover::describe(UpgradeRefusal::InProgress));
    }
    SECTION("an exec failure names no refusal and keeps the daemon's cause")
    {
        auto h = Upgrade {};
        h.control.reply = proto::UpgradeDaemonReply { .result = proto::UpgradeDaemonReply::ExecFailedResult,
                                                      .detail = "Permission denied" };
        auto const upgraded = vthost::client::upgradeDaemon(h.control, h.binary);
        REQUIRE_FALSE(upgraded.has_value());
        CHECK(upgraded.error().error == DaemonUpgradeError::Refused);
        CHECK_FALSE(upgraded.error().refusal.has_value());
        CHECK(upgraded.error().detail.starts_with(vthost::handover::ExecFailureText));
        CHECK(upgraded.error().detail.ends_with("Permission denied"));
    }
    SECTION("a refusal only a newer daemon knows")
    {
        auto h = Upgrade {};
        h.control.reply = proto::UpgradeDaemonReply { .result = 200, .detail = {} };
        auto const upgraded = vthost::client::upgradeDaemon(h.control, h.binary);
        REQUIRE_FALSE(upgraded.has_value());
        CHECK(upgraded.error().error == DaemonUpgradeError::Refused);
        CHECK_FALSE(upgraded.error().refusal.has_value());
        CHECK(upgraded.error().detail.contains("200"));
    }
    SECTION("a daemon that went away before it answered")
    {
        auto h = Upgrade {};
        h.control.upgradeError = ControlError { .code = ControlErrorCode::NotRunning, .detail = {} };
        auto const upgraded = vthost::client::upgradeDaemon(h.control, h.binary);
        REQUIRE_FALSE(upgraded.has_value());
        CHECK(upgraded.error().error == DaemonUpgradeError::NotRunning);
    }
}

TEST_CASE("every refusal and the exec failure have a text for the user", "[vthost][client][upgrade]")
{
    for (auto const index: std::views::iota(0, 1 + std::to_underlying(vthost::handover::lastRefusal())))
    {
        auto const refusal = static_cast<UpgradeRefusal>(index);
        INFO("refusal " << index);
        // Without the daemon's words, as an old daemon would answer.
        auto h = Upgrade {};
        h.control.reply =
            proto::UpgradeDaemonReply { .result = static_cast<uint8_t>(1 + index), .detail = {} };
        auto const upgraded = vthost::client::upgradeDaemon(h.control, h.binary);
        REQUIRE_FALSE(upgraded.has_value());
        CHECK(upgraded.error().refusal == refusal);
        CHECK_FALSE(vthost::client::describeFailure(upgraded.error()).empty());
        CHECK(vthost::client::describeFailure(upgraded.error()) == vthost::handover::describe(refusal));
    }
    auto h = Upgrade {};
    h.control.reply =
        proto::UpgradeDaemonReply { .result = proto::UpgradeDaemonReply::ExecFailedResult, .detail = {} };
    auto const upgraded = vthost::client::upgradeDaemon(h.control, h.binary);
    REQUIRE_FALSE(upgraded.has_value());
    CHECK(vthost::client::describeFailure(upgraded.error()) == vthost::handover::ExecFailureText);
}

namespace
{
/// Time that passes only when waited for.
class ManualPacer final: public vthost::client::SettlePacer
{
  public:
    [[nodiscard]] std::chrono::steady_clock::time_point now() const override { return _now; }
    [[nodiscard]] vthost::client::SettleWait pause(std::chrono::milliseconds duration) override
    {
        if (cancelled == vthost::client::SettleWait::Cancelled)
            return cancelled;
        _now += duration;
        waited += duration;
        return vthost::client::SettleWait::Elapsed;
    }

    std::chrono::milliseconds waited {}; ///< All the waiting done.
    /// What every wait ends in: Cancelled as a pacer whose stop was requested.
    vthost::client::SettleWait cancelled = vthost::client::SettleWait::Elapsed;

  private:
    std::chrono::steady_clock::time_point _now {};
};
} // namespace

TEST_CASE("upgradeDaemonTo settles an upgrade whose outcome is unknown", "[vthost][client][upgrade]")
{
    auto pacer = ManualPacer {};
    auto h = Upgrade {};
    h.control.upgradeError = ControlError { .code = ControlErrorCode::Transport, .detail = "broken" };

    SECTION("the daemon is back on another release after a brief absence: success")
    {
        h.control.laterStatuses = { std::nullopt, std::nullopt, "2.0.0" };
        CHECK(vthost::client::upgradeDaemonTo(h.control, h.binary, pacer).has_value());
        CHECK(pacer.waited > std::chrono::milliseconds { 0 });
    }
    SECTION("the daemon is back on the same release: the upgrade did not happen")
    {
        h.control.laterStatuses = { std::nullopt, "1.0.0" };
        auto const upgraded = vthost::client::upgradeDaemonTo(h.control, h.binary, pacer);
        REQUIRE_FALSE(upgraded.has_value());
        CHECK(upgraded.error().outcome == vthost::client::DaemonUpgradeOutcome::Known);
        CHECK(upgraded.error().detail.contains("1.0.0"));
    }
    SECTION("the daemon never comes back within the hold: still unknown")
    {
        h.control.laterStatuses = {};
        h.control.releaseAfterAsked = std::nullopt;
        // Absent for every ask: the script runs dry, so make each ask fail.
        h.control.laterStatuses.assign(1000, std::nullopt);
        auto const upgraded = vthost::client::upgradeDaemonTo(h.control, h.binary, pacer);
        REQUIRE_FALSE(upgraded.has_value());
        CHECK(upgraded.error().outcome == vthost::client::DaemonUpgradeOutcome::Unknown);
        CHECK(pacer.waited >= vthost::client::UpgradeHold);
        CHECK(pacer.waited < vthost::client::UpgradeHold + std::chrono::seconds { 6 });
    }
    SECTION("a daemon already on the target before the call is not mistaken for an upgraded one")
    {
        h.control.statusRelease = "2.0.0";
        h.control.laterStatuses = { "2.0.0" };
        auto const upgraded = vthost::client::upgradeDaemonTo(h.control, h.binary, pacer);
        REQUIRE_FALSE(upgraded.has_value());
        CHECK(upgraded.error().outcome == vthost::client::DaemonUpgradeOutcome::Known);
    }
}

TEST_CASE("upgradeDaemonTo does not ask again after a refusal", "[vthost][client][upgrade]")
{
    auto pacer = ManualPacer {};
    auto h = Upgrade {};
    h.control.reply = proto::UpgradeDaemonReply { .result = 1, .detail = {} };
    auto const upgraded = vthost::client::upgradeDaemonTo(h.control, h.binary, pacer);
    REQUIRE_FALSE(upgraded.has_value());
    CHECK(upgraded.error().outcome == vthost::client::DaemonUpgradeOutcome::Known);
    CHECK(pacer.waited == std::chrono::milliseconds { 0 });
}

namespace
{
using vthost::client::DaemonUpgradeOutcome;

/// @return A record of @p generation, after @p attempts requests, with the failure @p result (as
/// UpgradeDaemonReply spells it) and its words.
[[nodiscard]] proto::DaemonUpgradeRecord recordOf(uint64_t generation,
                                                  uint8_t result = 0,
                                                  std::string detail = {},
                                                  uint64_t attempts = 1)
{
    return proto::DaemonUpgradeRecord { .generation = generation,
                                        .attempts = attempts,
                                        .lastFailure = result,
                                        .lastFailureDetail = std::move(detail) };
}

/// @return @p refusal as UpgradeDaemonReply::result spells it.
[[nodiscard]] uint8_t resultOf(UpgradeRefusal refusal)
{
    return static_cast<uint8_t>(1 + std::to_underlying(refusal));
}
} // namespace

TEST_CASE("upgradeDaemonTo takes a started upgrade as provisional and settles it by the generation",
          "[vthost][client][upgrade]")
{
    auto pacer = ManualPacer {};
    auto h = Upgrade {};
    h.control.statusRecord = recordOf(4, 0, {}, 0);

    SECTION("the new image serves: one generation further, on the same release")
    {
        h.control.laterStatuses = { std::nullopt, "1.0.0" };
        h.control.laterRecords = { recordOf(5) };
        CHECK(vthost::client::upgradeDaemonTo(h.control, h.binary, pacer).has_value());
    }
    SECTION("the daemon refused as it quiesced: a known failure, with its reason")
    {
        h.control.laterStatuses = { "1.0.0" };
        h.control.laterRecords = { recordOf(
            4,
            resultOf(UpgradeRefusal::QuiesceTimedOut),
            std::string { vthost::handover::describe(UpgradeRefusal::QuiesceTimedOut) }) };
        auto const upgraded = vthost::client::upgradeDaemonTo(h.control, h.binary, pacer);
        REQUIRE_FALSE(upgraded.has_value());
        CHECK(upgraded.error().outcome == DaemonUpgradeOutcome::Known);
        CHECK(upgraded.error().error == DaemonUpgradeError::Refused);
        CHECK(upgraded.error().refusal == UpgradeRefusal::QuiesceTimedOut);
        CHECK(vthost::client::describeFailure(upgraded.error())
              == vthost::handover::describe(UpgradeRefusal::QuiesceTimedOut));
    }
    SECTION("the exec failed: a known failure, naming it")
    {
        h.control.laterStatuses = { "1.0.0" };
        h.control.laterRecords = { recordOf(
            4, proto::UpgradeDaemonReply::ExecFailedResult, "Exec format error") };
        auto const upgraded = vthost::client::upgradeDaemonTo(h.control, h.binary, pacer);
        REQUIRE_FALSE(upgraded.has_value());
        CHECK(upgraded.error().outcome == DaemonUpgradeOutcome::Known);
        CHECK_FALSE(upgraded.error().refusal.has_value());
        CHECK(upgraded.error().detail.starts_with(vthost::handover::ExecFailureText));
        CHECK(upgraded.error().detail.ends_with("Exec format error"));
    }
    SECTION("the daemon says nothing conclusive by the end of the hold: unknown")
    {
        h.control.laterStatuses.assign(1000, "1.0.0");
        auto const upgraded = vthost::client::upgradeDaemonTo(h.control, h.binary, pacer);
        REQUIRE_FALSE(upgraded.has_value());
        CHECK(upgraded.error().outcome == DaemonUpgradeOutcome::Unknown);
        CHECK(pacer.waited >= vthost::client::UpgradeHold);
    }
    SECTION("the settling is cancelled: unknown, at once")
    {
        pacer.cancelled = vthost::client::SettleWait::Cancelled;
        h.control.laterStatuses = { "1.0.0" };
        h.control.laterRecords = { recordOf(5) };
        auto const upgraded = vthost::client::upgradeDaemonTo(h.control, h.binary, pacer);
        REQUIRE_FALSE(upgraded.has_value());
        CHECK(upgraded.error().outcome == DaemonUpgradeOutcome::Unknown);
        CHECK(pacer.waited == std::chrono::milliseconds { 0 });
    }
}

TEST_CASE("upgradeDaemonTo does not take an earlier request's failure for its own",
          "[vthost][client][upgrade]")
{
    auto pacer = ManualPacer {};
    auto h = Upgrade {};
    auto const stale = std::string { vthost::handover::describe(UpgradeRefusal::QuiesceTimedOut) };
    // An earlier request failed after its announcement, and the daemon still records it.
    h.control.statusRecord = recordOf(4, resultOf(UpgradeRefusal::QuiesceTimedOut), stale, 1);
    // This request's answer was lost, so its outcome is unknown and the settling polls.
    h.control.upgradeError = ControlError { .code = ControlErrorCode::Transport, .detail = "broken" };

    SECTION("the stale failure is not reported while the attempts have not advanced")
    {
        h.control.laterStatuses.assign(1000, "1.0.0");
        auto const upgraded = vthost::client::upgradeDaemonTo(h.control, h.binary, pacer);
        REQUIRE_FALSE(upgraded.has_value());
        CHECK(upgraded.error().outcome == DaemonUpgradeOutcome::Unknown);
        CHECK(pacer.waited >= vthost::client::UpgradeHold);
    }
    SECTION("the stale failure is overtaken by the upgrade it did not belong to")
    {
        h.control.laterStatuses = { "1.0.0", "1.0.0" };
        h.control.laterRecords = { recordOf(4, resultOf(UpgradeRefusal::QuiesceTimedOut), stale, 1),
                                   recordOf(5, 0, {}, 2) };
        CHECK(vthost::client::upgradeDaemonTo(h.control, h.binary, pacer).has_value());
    }
    SECTION("a failure of this request is reported, even with the code and words of the earlier one")
    {
        h.control.laterStatuses = { "1.0.0" };
        h.control.laterRecords = { recordOf(4, resultOf(UpgradeRefusal::QuiesceTimedOut), stale, 2) };
        auto const upgraded = vthost::client::upgradeDaemonTo(h.control, h.binary, pacer);
        REQUIRE_FALSE(upgraded.has_value());
        CHECK(upgraded.error().outcome == DaemonUpgradeOutcome::Known);
        CHECK(upgraded.error().refusal == UpgradeRefusal::QuiesceTimedOut);
    }
}

TEST_CASE("upgradeDaemonTo names the release the daemon serves", "[vthost][client][upgrade]")
{
    auto pacer = ManualPacer {};
    auto h = Upgrade {};
    h.control.statusRecord = recordOf(4, 0, {}, 0);
    h.control.laterStatuses = { "3.0.0" };
    h.control.laterRecords = { recordOf(5) };
    auto const upgraded = vthost::client::upgradeDaemonTo(h.control, h.binary, pacer);
    REQUIRE(upgraded.has_value());
    CHECK(upgraded->release == "3.0.0");
}

TEST_CASE("upgradeDaemonTo settles even where the status before could not be read",
          "[vthost][client][upgrade]")
{
    auto pacer = ManualPacer {};
    auto h = Upgrade {};
    h.control.statusRecord = recordOf(4, 0, {}, 0);

    SECTION("a second read succeeds: the baseline is there")
    {
        h.control.statusesToFail = 1;
        h.control.laterStatuses = { "1.0.0" };
        h.control.laterRecords = { recordOf(5) };
        CHECK(vthost::client::upgradeDaemonTo(h.control, h.binary, pacer).has_value());
    }
    SECTION("no baseline, and the daemon states a clean record: success")
    {
        h.control.statusesToFail = 2;
        h.control.laterStatuses = { "1.0.0" };
        h.control.laterRecords = { recordOf(5) };
        auto const upgraded = vthost::client::upgradeDaemonTo(h.control, h.binary, pacer);
        REQUIRE(upgraded.has_value());
        CHECK(upgraded->release == "1.0.0");
    }
    SECTION("no baseline, and the daemon states a failure: that failure")
    {
        h.control.statusesToFail = 2;
        h.control.laterStatuses = { "1.0.0" };
        h.control.laterRecords = { recordOf(4, resultOf(UpgradeRefusal::QuiesceTimedOut), "x", 1) };
        auto const upgraded = vthost::client::upgradeDaemonTo(h.control, h.binary, pacer);
        REQUIRE_FALSE(upgraded.has_value());
        CHECK(upgraded.error().outcome == DaemonUpgradeOutcome::Known);
    }
    SECTION("no baseline, and nothing is ever stated: unknown after the hold")
    {
        h.control.statusesToFail = 2;
        h.control.statusRecord = std::nullopt;
        h.control.laterStatuses.assign(1000, "1.0.0");
        auto const upgraded = vthost::client::upgradeDaemonTo(h.control, h.binary, pacer);
        REQUIRE_FALSE(upgraded.has_value());
        CHECK(upgraded.error().outcome == DaemonUpgradeOutcome::Unknown);
        CHECK(pacer.waited >= vthost::client::UpgradeHold);
    }
}

TEST_CASE("upgradeDaemonTo settles a same-release upgrade whose answer was lost by the generation",
          "[vthost][client][upgrade]")
{
    // The release cannot tell: before and after are both 1.0.0. Judged by the release, this read "did not
    // happen"; the generation says it did.
    auto pacer = ManualPacer {};
    auto h = Upgrade {};
    h.control.statusRecord = recordOf(0, 0, {}, 0);
    h.control.upgradeError = ControlError { .code = ControlErrorCode::Transport, .detail = "broken" };
    h.control.laterStatuses = { "1.0.0" };
    h.control.laterRecords = { recordOf(1) };
    CHECK(vthost::client::upgradeDaemonTo(h.control, h.binary, pacer).has_value());
}

TEST_CASE("upgradeDaemonTo takes an older daemon's started at its word", "[vthost][client][upgrade]")
{
    // A daemon from before the record cannot say how its upgrade ended.
    auto pacer = ManualPacer {};
    auto h = Upgrade {};
    CHECK(vthost::client::upgradeDaemonTo(h.control, h.binary, pacer).has_value());
    CHECK(pacer.waited == std::chrono::milliseconds { 0 });
}

TEST_CASE("SystemSettlePacer's wait ends at once when its stop is requested", "[vthost][client][upgrade]")
{
    auto source = std::stop_source {};
    auto pacer = vthost::client::SystemSettlePacer { source.get_token() };
    CHECK(pacer.pause(std::chrono::milliseconds { 1 }) == vthost::client::SettleWait::Elapsed);
    source.request_stop();
    auto const start = std::chrono::steady_clock::now();
    CHECK(pacer.pause(std::chrono::hours { 1 }) == vthost::client::SettleWait::Cancelled);
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds { 10 });
}
