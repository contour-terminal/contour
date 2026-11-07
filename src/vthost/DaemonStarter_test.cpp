// SPDX-License-Identifier: Apache-2.0
#include <vtpty/ProcessPlacement.hpp>

#include <core/platform/testing/InMemoryFileSystem.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <vthost/DaemonStarter.hpp>
#include <vthost/testing/FakeServiceBackend.hpp>

namespace fs = std::filesystem;

using vthost::DaemonLaunchStyle;
using vthost::DaemonPresence;
using vthost::ServiceRunState;
using vthost::testing::FakeServiceBackend;
using vthost::testing::FakeServiceLog;

namespace
{
/// A DaemonSpawner that starts nothing: it counts spawns and answers each probe as told.
class ScriptedSpawner final: public vthost::DaemonSpawner
{
  public:
    [[nodiscard]] std::expected<void, std::string> spawn(std::span<std::string const>,
                                                         vtpty::ProcessPlacement&) override
    {
        ++spawns;
        return {};
    }

    [[nodiscard]] DaemonPresence probe(fs::path const&) override { return presence; }

    DaemonPresence presence = DaemonPresence::Absent;
    int spawns = 0;
};

/// A DaemonBinarySource that counts how often it is asked.
class CountingBinary final: public vthost::DaemonBinarySource
{
  public:
    [[nodiscard]] vthost::StagedImage imageForSpawn() override
    {
        ++calls;
        return vthost::StagedImage { .executable = "contour-no-such-binary",
                                     .lease = nullptr,
                                     .origin = vthost::ImageOrigin::Installed,
                                     .imageId = {} };
    }

    int calls = 0;
};

constexpr auto SocketPath = "/nonexistent/contour/control";

/// A registration found on @p backend, with the status it reports.
[[nodiscard]] vthost::RegisteredService registered(std::unique_ptr<FakeServiceBackend> backend,
                                                   std::string name = "contour-daemon")
{
    auto status = backend->status().value();
    return vthost::RegisteredService { .backend = std::move(backend),
                                       .name = std::move(name),
                                       .status = std::move(status) };
}

struct OnDemandFixture
{
    ScriptedSpawner spawner {};
    CountingBinary binary {};
    vtpty::NoPlacement placement {};

    [[nodiscard]] vthost::OnDemandSpawn spawn(std::chrono::seconds timeout)
    {
        return vthost::OnDemandSpawn { .endpoint = vthost::UnixEndpoint { .socketPath = SocketPath },
                                       .spawner = spawner,
                                       .binary = binary,
                                       .placement = placement,
                                       .options = {},
                                       .timeout = timeout };
    }
};
} // namespace

TEST_CASE("a live probe decides whether a registered daemon runs", "[vthost][starter]")
{
    using vthost::effectiveRunState;
    CHECK(effectiveRunState(ServiceRunState::Stopped, DaemonPresence::Serving) == ServiceRunState::Running);
    CHECK(effectiveRunState(ServiceRunState::Running, DaemonPresence::Absent) == ServiceRunState::Stopped);
    CHECK(effectiveRunState(ServiceRunState::NotInstalled, DaemonPresence::Serving)
          == ServiceRunState::NotInstalled);
    CHECK(effectiveRunState(ServiceRunState::NotInstalled, DaemonPresence::Absent)
          == ServiceRunState::NotInstalled);
}

TEST_CASE("only a self-detaching service leaves its daemon to the daemon's own stop", "[vthost][starter]")
{
    CHECK(vthost::stopVia(DaemonLaunchStyle::Foreground) == vthost::StopRoute::ServiceManager);
    CHECK(vthost::stopVia(DaemonLaunchStyle::SelfDetaching) == vthost::StopRoute::DaemonControl);
}

TEST_CASE("a registered service is started instead of spawning on demand", "[vthost][starter]")
{
    auto log = FakeServiceLog {};
    auto fixture = OnDemandFixture {};
    // The daemon appears once the service has been started.
    fixture.spawner.presence = DaemonPresence::Serving;

    auto starter = vthost::chooseDaemonStarter(
        registered(std::make_unique<FakeServiceBackend>(ServiceRunState::Stopped, log)),
        SocketPath,
        fixture.spawn(std::chrono::seconds { 1 }));
    REQUIRE(starter);
    CHECK(starter->start().has_value());
    CHECK(log.starts == 1);
    CHECK(fixture.binary.calls == 0);
    CHECK(fixture.spawner.spawns == 0);
}

TEST_CASE("a started service that never serves is an error naming the socket", "[vthost][starter]")
{
    auto log = FakeServiceLog {};
    auto fixture = OnDemandFixture {};
    auto starter = vthost::ServiceDaemonStarter(
        registered(std::make_unique<FakeServiceBackend>(ServiceRunState::Stopped, log)),
        fixture.spawner,
        SocketPath,
        std::chrono::milliseconds { 0 });
    auto const result = starter.start();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().contains(SocketPath));
    CHECK(log.starts == 1);
}

TEST_CASE("a registered service is not started for a socket it does not serve", "[vthost][starter]")
{
    auto log = FakeServiceLog {};
    auto fixture = OnDemandFixture {};
    // $CONTOUR_MUX or --socket points elsewhere than the label's default the service serves.
    auto starter = vthost::chooseDaemonStarter(
        registered(std::make_unique<FakeServiceBackend>(ServiceRunState::Stopped, log)),
        "/nonexistent/contour/default",
        fixture.spawn(std::chrono::seconds { 0 }));
    REQUIRE(starter);
    CHECK_FALSE(starter->start().has_value());
    CHECK(log.starts == 0);
    CHECK(fixture.spawner.spawns == 1);
    CHECK(fixture.binary.calls == 1);
}

TEST_CASE("a registration that pins its socket is compared by that socket", "[vthost][starter]")
{
    auto log = FakeServiceLog {};
    auto fixture = OnDemandFixture {};
    fixture.spawner.presence = DaemonPresence::Serving;
    auto const pinned = std::string { "/usr/bin/contour daemon --socket=" } + SocketPath;
    auto starter = vthost::chooseDaemonStarter(
        registered(std::make_unique<FakeServiceBackend>(
            ServiceRunState::Stopped, log, DaemonLaunchStyle::Foreground, pinned)),
        "/nonexistent/contour/default",
        fixture.spawn(std::chrono::seconds { 1 }));
    REQUIRE(starter);
    CHECK(starter->start().has_value());
    CHECK(log.starts == 1);
    CHECK(fixture.spawner.spawns == 0);
}

TEST_CASE("socket paths compare regardless of separators and dot segments", "[vthost][starter]")
{
    CHECK(vthost::isSameSocketPath("/nonexistent/a/b", "/nonexistent/a/b"));
    CHECK(vthost::isSameSocketPath("/nonexistent/a/b", "/nonexistent/a/b/"));
    CHECK(vthost::isSameSocketPath("/nonexistent/a/./c/../b", "/nonexistent/a/b"));
    CHECK_FALSE(vthost::isSameSocketPath("/nonexistent/a/b", "/nonexistent/a/c"));
}

TEST_CASE("without a registration the daemon is spawned on demand", "[vthost][starter]")
{
    auto fixture = OnDemandFixture {};
    auto starter =
        vthost::chooseDaemonStarter(std::nullopt, SocketPath, fixture.spawn(std::chrono::seconds { 0 }));
    REQUIRE(starter);
    auto const result = starter->start();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().contains(SocketPath));
    CHECK(fixture.spawner.spawns == 1);
    CHECK(fixture.binary.calls == 1);
}

TEST_CASE("findRegisteredService finds a legacy-named registration", "[vthost][starter]")
{
    auto log = FakeServiceLog {};
    auto asked = std::vector<std::string> {};
    auto const factory = vthost::ServiceBackendFactory { [&](vthost::ServiceStartMode, std::string_view name)
                                                             -> std::unique_ptr<vthost::ServiceBackend> {
        asked.emplace_back(name);
        return std::make_unique<FakeServiceBackend>(
            name == "contour-daemon-default" ? ServiceRunState::Stopped : ServiceRunState::NotInstalled, log);
    } };

    auto const found = vthost::findRegisteredService("default", factory);
    REQUIRE(found.has_value());
    CHECK(found->name == "contour-daemon-default");
    CHECK(found->backend != nullptr);
    // The current name is asked (under every mode) before the legacy one.
    REQUIRE_FALSE(asked.empty());
    CHECK(asked.front() == "contour-daemon");
    CHECK(asked.back() == "contour-daemon-default");
}

TEST_CASE("findRegisteredService reports nothing when no name is registered", "[vthost][starter]")
{
    auto log = FakeServiceLog {};
    auto const factory =
        vthost::ServiceBackendFactory { [&](vthost::ServiceStartMode,
                                            std::string_view) -> std::unique_ptr<vthost::ServiceBackend> {
            return std::make_unique<FakeServiceBackend>(ServiceRunState::NotInstalled, log);
        } };
    CHECK_FALSE(vthost::findRegisteredService("default", factory).has_value());
}

namespace
{
constexpr auto GonePath = "C:/Program Files/Contour Terminal Emulator 0.8/bin/contour.exe";
constexpr auto InstalledPath = "C:/Program Files/Contour/bin/contour.exe";

/// A file system in which the installed executable exists and nothing else does.
[[nodiscard]] core::platform::testing::InMemoryFileSystem installedFileSystem()
{
    return core::platform::testing::InMemoryFileSystem { { .path = InstalledPath, .content = "MZ" } };
}

/// A registration of @p program on a fake that records what it is retargeted at.
[[nodiscard]] std::unique_ptr<FakeServiceBackend> registrationOf(fs::path program, FakeServiceLog& log)
{
    return std::make_unique<FakeServiceBackend>(
        ServiceRunState::Stopped, log, DaemonLaunchStyle::SelfDetaching, std::string {}, std::move(program));
}
} // namespace

TEST_CASE("a registration whose program is gone is pointed at the installed executable",
          "[vthost][starter][daemonstarter]")
{
    auto log = FakeServiceLog {};
    auto service = registered(registrationOf(GonePath, log));
    auto const files = installedFileSystem();

    auto const repaired = vthost::repairRegistration(
        service,
        vthost::RegistrationRepairInputs { .fileSystem = files, .installedExecutable = InstalledPath });

    REQUIRE(repaired.has_value());
    CHECK(*repaired == vthost::RegistrationRepair::Retargeted);
    REQUIRE(log.retargets.size() == 1);
    CHECK(log.retargets.front() == fs::path { InstalledPath });
}

TEST_CASE("a registration whose program exists is left alone", "[vthost][starter][daemonstarter]")
{
    auto log = FakeServiceLog {};
    auto service = registered(registrationOf(InstalledPath, log));
    auto const files = installedFileSystem();

    auto const repaired = vthost::repairRegistration(
        service,
        vthost::RegistrationRepairInputs { .fileSystem = files, .installedExecutable = InstalledPath });

    REQUIRE(repaired.has_value());
    CHECK(*repaired == vthost::RegistrationRepair::Unchanged);
    CHECK(log.retargets.empty());
}

TEST_CASE("a registration that names no program is left alone", "[vthost][starter][daemonstarter]")
{
    auto log = FakeServiceLog {};
    auto service = registered(registrationOf({}, log));
    auto const files = installedFileSystem();

    auto const repaired = vthost::repairRegistration(
        service,
        vthost::RegistrationRepairInputs { .fileSystem = files, .installedExecutable = InstalledPath });

    REQUIRE(repaired.has_value());
    CHECK(*repaired == vthost::RegistrationRepair::Unchanged);
    CHECK(log.retargets.empty());
}

TEST_CASE("nothing is retargeted at an installed executable that is missing too",
          "[vthost][starter][daemonstarter]")
{
    auto log = FakeServiceLog {};
    auto service = registered(registrationOf(GonePath, log));
    auto const nothingInstalled = core::platform::testing::InMemoryFileSystem {};

    auto const repaired =
        vthost::repairRegistration(service,
                                   vthost::RegistrationRepairInputs { .fileSystem = nothingInstalled,
                                                                      .installedExecutable = InstalledPath });

    REQUIRE(repaired.has_value());
    CHECK(*repaired == vthost::RegistrationRepair::Unchanged);
    CHECK(log.retargets.empty());
}

TEST_CASE("a retarget the backend refuses is an error, not a throw", "[vthost][starter][daemonstarter]")
{
    auto log = FakeServiceLog {};
    auto service = registered(std::make_unique<FakeServiceBackend>(
        ServiceRunState::Stopped,
        log,
        DaemonLaunchStyle::SelfDetaching,
        std::string {},
        fs::path { GonePath },
        vthost::ServiceError {
            .code = vthost::ServiceErrorCode::Backend, .systemCode = 5, .context = "retarget" }));
    auto const files = installedFileSystem();

    auto const repaired = vthost::repairRegistration(
        service,
        vthost::RegistrationRepairInputs { .fileSystem = files, .installedExecutable = InstalledPath });

    REQUIRE_FALSE(repaired.has_value());
    CHECK(repaired.error().code == vthost::ServiceErrorCode::Backend);
    CHECK(repaired.error().systemCode == 5);
}

TEST_CASE("findRepairedRegisteredService returns the registration after repairing it",
          "[vthost][starter][daemonstarter]")
{
    auto log = FakeServiceLog {};
    auto const files = installedFileSystem();
    auto const factory =
        vthost::ServiceBackendFactory { [&](vthost::ServiceStartMode,
                                            std::string_view) -> std::unique_ptr<vthost::ServiceBackend> {
            return registrationOf(GonePath, log);
        } };

    auto const found = vthost::findRepairedRegisteredService(
        "default",
        factory,
        vthost::RegistrationRepairInputs { .fileSystem = files, .installedExecutable = InstalledPath },
        vthost::RegistrationUse::Manage);

    REQUIRE(found.has_value());
    CHECK(log.retargets.size() == 1);
}

TEST_CASE("findRepairedRegisteredService returns a registration whose repair failed as found",
          "[vthost][starter][daemonstarter]")
{
    auto log = FakeServiceLog {};
    auto const files = installedFileSystem();
    auto const factory =
        vthost::ServiceBackendFactory { [&](vthost::ServiceStartMode,
                                            std::string_view) -> std::unique_ptr<vthost::ServiceBackend> {
            return std::make_unique<FakeServiceBackend>(
                ServiceRunState::Stopped,
                log,
                DaemonLaunchStyle::SelfDetaching,
                std::string {},
                fs::path { GonePath },
                vthost::ServiceError { .code = vthost::ServiceErrorCode::Backend, .systemCode = 5 });
        } };

    auto const found = vthost::findRepairedRegisteredService(
        "default",
        factory,
        vthost::RegistrationRepairInputs { .fileSystem = files, .installedExecutable = InstalledPath },
        vthost::RegistrationUse::Manage);

    REQUIRE(found.has_value());
    CHECK(log.retargets.empty());
}

TEST_CASE("a disabled registration stays disabled whatever the live probe says", "[vthost][starter]")
{
    CHECK(vthost::effectiveRunState(ServiceRunState::Disabled, DaemonPresence::Serving)
          == ServiceRunState::Disabled);
    CHECK(vthost::effectiveRunState(ServiceRunState::Disabled, DaemonPresence::Absent)
          == ServiceRunState::Disabled);
}

TEST_CASE("a disabled registration is not started: the daemon is spawned on demand", "[vthost][starter]")
{
    auto log = FakeServiceLog {};
    auto fixture = OnDemandFixture {};
    auto starter = vthost::chooseDaemonStarter(
        registered(std::make_unique<FakeServiceBackend>(ServiceRunState::Disabled, log)),
        SocketPath,
        fixture.spawn(std::chrono::seconds { 0 }));
    REQUIRE(starter);
    CHECK_FALSE(starter->start().has_value());
    CHECK(log.starts == 0);
    CHECK(fixture.spawner.spawns == 1);
}

namespace
{
/// An InstallRootSource answering from a table.
class FakeInstallRoots final: public vthost::InstallRootSource
{
  public:
    FakeInstallRoots(std::optional<std::string> machine, std::optional<std::string> user = std::nullopt):
        _machine { std::move(machine) }, _user { std::move(user) }
    {
    }

    [[nodiscard]] std::optional<std::string> recordedRoot(vthost::InstallRootHive hive) const override
    {
        return hive == vthost::InstallRootHive::LocalMachine ? _machine : _user;
    }

  private:
    std::optional<std::string> _machine;
    std::optional<std::string> _user;
};
} // namespace

TEST_CASE("the installed executable is the one in the recorded install folder",
          "[vthost][starter][installed]")
{
    auto const files = installedFileSystem();
    CHECK(vthost::installedExecutableOf(FakeInstallRoots { "C:/Program Files/Contour" }, files)
          == fs::path { InstalledPath });
    // The per-user record, where the machine has none.
    CHECK(vthost::installedExecutableOf(FakeInstallRoots { std::nullopt, "C:/Program Files/Contour/" }, files)
          == fs::path { InstalledPath });
    // A record whose executable is gone, or none at all: nothing is installed.
    CHECK(vthost::installedExecutableOf(FakeInstallRoots { "C:/Gone" }, files).empty());
    CHECK(vthost::installedExecutableOf(FakeInstallRoots { std::nullopt }, files).empty());
}

TEST_CASE("a running executable outside the recorded install folder does not retarget a registration",
          "[vthost][starter][installed]")
{
    // A portable copy or a build tree is running; the installer recorded a folder with no executable in it.
    auto log = FakeServiceLog {};
    auto service = registered(registrationOf(GonePath, log));
    auto const runningCopy = core::platform::testing::InMemoryFileSystem {
        { .path = "D:/work/out/build/msvc-release/bin/contour.exe", .content = "MZ" }
    };
    auto const inputs = vthost::RegistrationRepairInputs {
        .fileSystem = runningCopy,
        .installedExecutable =
            vthost::installedExecutableOf(FakeInstallRoots { "C:/Program Files/Contour" }, runningCopy)
    };

    auto const repaired = vthost::repairRegistration(service, inputs);

    REQUIRE(repaired.has_value());
    CHECK(*repaired == vthost::RegistrationRepair::Unchanged);
    CHECK(log.retargets.empty());
}

TEST_CASE("a client gets no registration whose program is still missing after the repair",
          "[vthost][starter][daemonstarter]")
{
    auto const files = installedFileSystem();
    auto const refused =
        vthost::ServiceError { .code = vthost::ServiceErrorCode::AccessDenied, .systemCode = 5 };
    auto log = FakeServiceLog {};
    auto const factory =
        vthost::ServiceBackendFactory { [&](vthost::ServiceStartMode,
                                            std::string_view) -> std::unique_ptr<vthost::ServiceBackend> {
            return std::make_unique<FakeServiceBackend>(ServiceRunState::Stopped,
                                                        log,
                                                        DaemonLaunchStyle::SelfDetaching,
                                                        std::string {},
                                                        fs::path { GonePath },
                                                        refused);
        } };
    auto const inputs =
        vthost::RegistrationRepairInputs { .fileSystem = files, .installedExecutable = InstalledPath };

    // A client falls back to spawning the daemon; the verbs still get the registration, to remove it.
    CHECK_FALSE(
        vthost::findRepairedRegisteredService("default", factory, inputs, vthost::RegistrationUse::Start)
            .has_value());
    CHECK(vthost::findRepairedRegisteredService("default", factory, inputs, vthost::RegistrationUse::Manage)
              .has_value());
}

TEST_CASE("a client gets a registration whose program exists", "[vthost][starter][daemonstarter]")
{
    auto const files = installedFileSystem();
    auto log = FakeServiceLog {};
    auto const factory =
        vthost::ServiceBackendFactory { [&](vthost::ServiceStartMode,
                                            std::string_view) -> std::unique_ptr<vthost::ServiceBackend> {
            return registrationOf(InstalledPath, log);
        } };
    CHECK(vthost::findRepairedRegisteredService(
              "default",
              factory,
              vthost::RegistrationRepairInputs { .fileSystem = files, .installedExecutable = {} },
              vthost::RegistrationUse::Start)
              .has_value());
}

TEST_CASE("a repair leaves the status it carries current", "[vthost][starter][daemonstarter]")
{
    auto log = FakeServiceLog {};
    auto service = registered(registrationOf(GonePath, log));
    auto const files = installedFileSystem();
    REQUIRE(vthost::repairRegistration(service,
                                       vthost::RegistrationRepairInputs {
                                           .fileSystem = files, .installedExecutable = InstalledPath })
                .has_value());
    CHECK(service.status.program == fs::path { InstalledPath });
}
