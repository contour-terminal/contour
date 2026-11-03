// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <vthost/DaemonBinary.hpp>
#include <vthost/client/DaemonUpgrade.hpp>
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
        if (statusError)
            return std::unexpected(*statusError);
        auto status = vthost::client::DaemonStatus {};
        status.releaseVersion = "1.0.0";
        status.capabilities = offered;
        return status;
    }

    [[nodiscard]] std::expected<void, ControlError> stop(proto::StopReason) override { return {}; }

    [[nodiscard]] std::expected<proto::UpgradeDaemonReply, ControlError> upgrade(std::string imageId) override
    {
        upgrades.push_back(std::move(imageId));
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
        CHECK(upgraded.error().detail == "Permission denied");
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
