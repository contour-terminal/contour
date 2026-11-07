// SPDX-License-Identifier: Apache-2.0
//
// `contour daemon status | logs | stop`, driven over a fake control port, a fake prompt and an in-memory file
// system.

#include <contour/cli/DaemonVerbs.hpp>
#include <contour/config/Actions.hpp>
#include <contour/config/Config.hpp>

#include <core/platform/testing/InMemoryFileSystem.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <vthost/DaemonBinary.hpp>
#include <vthost/SessionSnapshotStore.hpp>
#include <vthost/client/DaemonControlClient.hpp>
#include <vthost/handover/UpgradeError.hpp>
#include <vthost/proto/Negotiation.hpp>
#include <vthost/testing/RecordingDurableFiles.hpp>
#include <vthost/testing/RecordingFilePrivacy.hpp>

namespace
{

using vthost::client::ControlError;
using vthost::client::ControlErrorCode;
using vthost::client::DaemonStatus;

/// A control port answering what it was given.
class FakeControlPort final: public vthost::client::DaemonControlPort
{
  public:
    explicit FakeControlPort(DaemonStatus reply): _result { std::move(reply) } {}

    explicit FakeControlPort(ControlErrorCode code, std::string detail = {}):
        _result { std::unexpected(ControlError { .code = code, .detail = std::move(detail) }) }
    {
    }

    [[nodiscard]] std::expected<DaemonStatus, ControlError> status() override { return _result; }

    [[nodiscard]] std::expected<void, ControlError> stop(vthost::proto::StopReason reason) override
    {
        stops.push_back(reason);
        if (!_result)
            return std::unexpected(_result.error());
        return {};
    }

    [[nodiscard]] std::expected<vthost::proto::UpgradeDaemonReply, ControlError> upgrade(std::string) override
    {
        return vthost::proto::UpgradeDaemonReply {};
    }

    std::vector<vthost::proto::StopReason> stops;

  private:
    std::expected<DaemonStatus, ControlError> _result;
};

/// A source of one staged image, with no lease.
class FakeBinarySource final: public vthost::DaemonBinarySource
{
  public:
    [[nodiscard]] vthost::StagedImage imageForSpawn() override
    {
        return vthost::StagedImage { .executable = "/state/bin/contour",
                                     .lease = nullptr,
                                     .origin = vthost::ImageOrigin::Staged,
                                     .imageId = "2.0.0-abcd" };
    }
};

/// A daemon offering a live upgrade, answering the upgrade as told.
class UpgradablePort final: public vthost::client::DaemonControlPort
{
  public:
    [[nodiscard]] std::expected<DaemonStatus, ControlError> status() override
    {
        if (notRunning || (errored && absentAfterError))
            return std::unexpected(ControlError { .code = ControlErrorCode::NotRunning, .detail = {} });
        if (errored && releaseAfterError)
            release = *releaseAfterError;
        auto status = DaemonStatus {};
        status.releaseVersion = release;
        status.capabilities = vthost::proto::CapabilitySet::of(
            { vthost::proto::Capability::DaemonControl, vthost::proto::Capability::LiveUpgrade });
        status.upgrade = record;
        return status;
    }

    [[nodiscard]] std::expected<void, ControlError> stop(vthost::proto::StopReason) override { return {}; }

    [[nodiscard]] std::expected<vthost::proto::UpgradeDaemonReply, ControlError> upgrade(std::string) override
    {
        if (error)
        {
            errored = true;
            return std::unexpected(*error);
        }
        release = upgradedRelease;
        if (recordAfterUpgrade)
            record = *recordAfterUpgrade;
        return reply;
    }

    /// The upgrade record status() reports; nullopt as a daemon from before it.
    std::optional<vthost::proto::DaemonUpgradeRecord> record = std::nullopt;
    /// The record it reports once it answered the upgrade.
    std::optional<vthost::proto::DaemonUpgradeRecord> recordAfterUpgrade = std::nullopt;

    bool errored = false;                             ///< Whether the upgrade failed with `error`.
    bool absentAfterError = false;                    ///< Whether no daemon answers after that.
    std::optional<std::string> releaseAfterError;     ///< The release it answers with after that.
    bool notRunning = false;                          ///< Whether no daemon listens.
    std::string release = "1.0.0";                    ///< The release status() reports.
    std::string upgradedRelease = "2.0.0";            ///< What it reports once upgraded.
    vthost::proto::UpgradeDaemonReply reply {};       ///< The answer to the upgrade.
    std::optional<ControlError> error = std::nullopt; ///< What the upgrade fails with, if it does.
};

/// Time that passes only when waited for.
class ManualPacer final: public vthost::client::SettlePacer
{
  public:
    [[nodiscard]] std::chrono::steady_clock::time_point now() const override { return _now; }
    [[nodiscard]] vthost::client::SettleWait pause(std::chrono::milliseconds duration) override
    {
        _now += duration;
        return vthost::client::SettleWait::Elapsed;
    }

  private:
    std::chrono::steady_clock::time_point _now {};
};

/// A prompt giving one canned answer, remembering the question.
class FakePrompt final: public contour::cli::ConfirmPrompt
{
  public:
    explicit FakePrompt(contour::cli::Confirmation answer): _answer { answer } {}

    [[nodiscard]] contour::cli::Confirmation ask(std::string_view question) override
    {
        asked = question;
        return _answer;
    }

    std::string asked;

  private:
    contour::cli::Confirmation _answer;
};

DaemonStatus sampleStatus()
{
    return DaemonStatus { .releaseVersion = "0.8.1",
                          .processId = 4242,
                          .uptime = std::chrono::seconds { 7385 },
                          .socketPath = "/run/c/default",
                          .executablePath = "/usr/bin/contour",
                          .logFile = "/l.log",
                          .sessions = 3,
                          .clients = 1 };
}

} // namespace

TEST_CASE("uptime reads in at most three units", "[contour][cli][daemon]")
{
    using namespace std::chrono_literals;
    CHECK(contour::cli::formatUptime(45s) == "45s");
    CHECK(contour::cli::formatUptime(125s) == "2m 05s");
    CHECK(contour::cli::formatUptime(7385s) == "2h 03m 05s");
    CHECK(contour::cli::formatUptime(273600s) == "3d 04h 00m");
}

TEST_CASE("status names version, uptime, socket, sessions and clients", "[contour][cli][daemon]")
{
    auto port = FakeControlPort { sampleStatus() };
    auto out = std::ostringstream {};
    auto err = std::ostringstream {};
    CHECK(contour::cli::daemonStatusVerb(port, "/run/c/default", out, err) == contour::cli::ExitStatus::Ok);
    for (auto const* needle:
         { "running (pid 4242)", "0.8.1", "2h 03m 05s", "/run/c/default", "sessions:", "3", "clients:" })
        CHECK(out.str().contains(needle));
}

TEST_CASE("status says standard error for a daemon with no log file", "[contour][cli][daemon]")
{
    auto status = sampleStatus();
    status.logFile.clear();
    CHECK(contour::cli::formatDaemonStatus(status).contains("log"));
    CHECK(contour::cli::formatDaemonStatus(status).contains("standard error"));
}

TEST_CASE("status of an absent daemon says so and exits 4 (unreachable)", "[contour][cli][daemon]")
{
    auto port = FakeControlPort { ControlErrorCode::NotRunning };
    auto out = std::ostringstream {};
    auto err = std::ostringstream {};
    CHECK(contour::cli::daemonStatusVerb(port, "/run/c/default", out, err)
          == contour::cli::ExitStatus::Unreachable);
    CHECK(contour::cli::exitCode(contour::cli::ExitStatus::Unreachable) == 4);
    CHECK(out.str() == "no daemon is running on /run/c/default\n");
}

TEST_CASE("status of a failing daemon reports on standard error", "[contour][cli][daemon]")
{
    auto port = FakeControlPort { ControlErrorCode::Timeout };
    auto out = std::ostringstream {};
    auto err = std::ostringstream {};
    CHECK(contour::cli::daemonStatusVerb(port, "/s", out, err) == contour::cli::ExitStatus::Failed);
    CHECK(out.str().empty());
    CHECK(!err.str().empty());
}

TEST_CASE("stop asks before ending sessions, and refuses without a terminal", "[contour][cli][daemon]")
{
    auto out = std::ostringstream {};
    auto err = std::ostringstream {};
    SECTION("declined")
    {
        auto port = FakeControlPort { sampleStatus() };
        auto prompt = FakePrompt { contour::cli::Confirmation::Declined };
        CHECK(contour::cli::daemonStopVerb(
                  port, "/run/c/default", { .consent = contour::cli::StopConsent::Ask }, prompt, out, err)
              == contour::cli::ExitStatus::Failed);
        CHECK(port.stops.empty());
        CHECK(prompt.asked.contains("3 session"));
        CHECK(err.str().contains("not stopped"));
    }
    SECTION("no terminal")
    {
        auto port = FakeControlPort { sampleStatus() };
        auto prompt = FakePrompt { contour::cli::Confirmation::Unavailable };
        CHECK(contour::cli::daemonStopVerb(
                  port, "/run/c/default", { .consent = contour::cli::StopConsent::Ask }, prompt, out, err)
              == contour::cli::ExitStatus::Failed);
        CHECK(port.stops.empty());
        CHECK(err.str().contains("--yes"));
    }
    SECTION("confirmed")
    {
        auto port = FakeControlPort { sampleStatus() };
        auto prompt = FakePrompt { contour::cli::Confirmation::Confirmed };
        CHECK(contour::cli::daemonStopVerb(
                  port, "/run/c/default", { .consent = contour::cli::StopConsent::Ask }, prompt, out, err)
              == contour::cli::ExitStatus::Ok);
        CHECK(port.stops.size() == 1);
    }
    SECTION("--yes")
    {
        auto port = FakeControlPort { sampleStatus() };
        auto prompt = FakePrompt { contour::cli::Confirmation::Declined };
        CHECK(contour::cli::daemonStopVerb(
                  port, "/run/c/default", { .consent = contour::cli::StopConsent::Given }, prompt, out, err)
              == contour::cli::ExitStatus::Ok);
        CHECK(port.stops == std::vector { vthost::proto::StopReason::User });
        CHECK(prompt.asked.empty());
    }
    SECTION("no sessions needs no consent")
    {
        auto status = sampleStatus();
        status.sessions = 0;
        auto port = FakeControlPort { status };
        auto prompt = FakePrompt { contour::cli::Confirmation::Declined };
        CHECK(contour::cli::daemonStopVerb(
                  port, "/run/c/default", { .consent = contour::cli::StopConsent::Ask }, prompt, out, err)
              == contour::cli::ExitStatus::Ok);
        CHECK(prompt.asked.empty());
    }
    SECTION("no daemon")
    {
        auto port = FakeControlPort { ControlErrorCode::NotRunning };
        auto prompt = FakePrompt { contour::cli::Confirmation::Confirmed };
        CHECK(contour::cli::daemonStopVerb(
                  port, "/run/c/default", { .consent = contour::cli::StopConsent::Given }, prompt, out, err)
              == contour::cli::ExitStatus::Unreachable);
        CHECK(err.str() == "no daemon is running on /run/c/default\n");
        CHECK(out.str().empty());
    }
    SECTION("a daemon without daemon control")
    {
        auto port = FakeControlPort { ControlErrorCode::Unsupported, "release 0.7.0" };
        auto prompt = FakePrompt { contour::cli::Confirmation::Confirmed };
        CHECK(contour::cli::daemonStopVerb(
                  port, "/run/c/default", { .consent = contour::cli::StopConsent::Given }, prompt, out, err)
              == contour::cli::ExitStatus::Failed);
        CHECK(err.str().contains("the daemon (release 0.7.0) does not support `contour daemon stop`"));
        CHECK(err.str().contains("Ctrl+C"));
    }
    SECTION("a daemon without daemon control that states no release")
    {
        auto port = FakeControlPort { ControlErrorCode::Unsupported, "an earlier release" };
        auto prompt = FakePrompt { contour::cli::Confirmation::Confirmed };
        CHECK(contour::cli::daemonStopVerb(
                  port, "/run/c/default", { .consent = contour::cli::StopConsent::Given }, prompt, out, err)
              == contour::cli::ExitStatus::Failed);
        CHECK(
            err.str().starts_with("the daemon (an earlier release) does not support `contour daemon stop`"));
    }
}

namespace
{

/// A saved workspace of one tab in an in-memory file, as the daemon labelled `default` keeps it.
struct SavedWorkspace
{
    vthost::testing::RecordingDurableFiles durability {};
    core::platform::testing::InMemoryFileSystem& files { durability.files };
    vthost::testing::RecordingFilePrivacy privacy { files };
    vthost::SessionSnapshotStore store {
        durability, privacy, std::filesystem::path { "/state/contour/sessions/default.snapshot" }
    };

    SavedWorkspace()
    {
        auto workspace = vthost::WorkspaceSnapshot {};
        workspace.windows.emplace_back().tabs.emplace_back();
        REQUIRE(store.write(workspace));
    }
};

} // namespace

TEST_CASE("daemon stop with no daemon running clears the saved workspace on consent",
          "[contour][cli][daemon]")
{
    auto saved = SavedWorkspace {};
    auto port = FakeControlPort { ControlErrorCode::NotRunning };
    auto out = std::ostringstream {};
    auto err = std::ostringstream {};
    auto const stop = [&](contour::cli::StopConsent consent, contour::cli::Confirmation answer) {
        auto prompt = FakePrompt { answer };
        auto const status = contour::cli::daemonStopVerb(
            port, "/run/c/default", { .consent = consent, .savedWorkspace = &saved.store }, prompt, out, err);
        return std::pair { status, prompt.asked };
    };

    SECTION("confirmed")
    {
        auto const [status, asked] =
            stop(contour::cli::StopConsent::Ask, contour::cli::Confirmation::Confirmed);
        CHECK(status == contour::cli::ExitStatus::Unreachable);
        CHECK(asked.contains("clear its saved workspace"));
        CHECK(saved.store.load().verdict == vthost::LoadVerdict::Empty);
        CHECK(err.str() == "no daemon is running; its saved workspace was cleared\n");
    }
    SECTION("declined")
    {
        auto const [status, asked] =
            stop(contour::cli::StopConsent::Ask, contour::cli::Confirmation::Declined);
        CHECK(status == contour::cli::ExitStatus::Unreachable);
        CHECK(saved.store.load().verdict == vthost::LoadVerdict::Restorable);
        CHECK(err.str() == "no daemon is running; its saved workspace is kept\n");
    }
    SECTION("no terminal to ask on")
    {
        auto const [status, asked] =
            stop(contour::cli::StopConsent::Ask, contour::cli::Confirmation::Unavailable);
        CHECK(status == contour::cli::ExitStatus::Failed);
        CHECK(saved.store.load().verdict == vthost::LoadVerdict::Restorable);
        CHECK(err.str().contains("--yes"));
    }
    SECTION("--yes")
    {
        auto const [status, asked] =
            stop(contour::cli::StopConsent::Given, contour::cli::Confirmation::Declined);
        CHECK(status == contour::cli::ExitStatus::Unreachable);
        CHECK(asked.empty());
        CHECK(saved.store.load().verdict == vthost::LoadVerdict::Empty);
    }
    CHECK(port.stops.empty());
}

TEST_CASE("daemon stop with no daemon running and nothing saved creates no file", "[contour][cli][daemon]")
{
    auto saved = SavedWorkspace {};
    auto const store = vthost::SessionSnapshotStore {
        saved.durability, saved.privacy, std::filesystem::path { "/state/contour/sessions/other.snapshot" }
    };
    auto port = FakeControlPort { ControlErrorCode::NotRunning };
    auto prompt = FakePrompt { contour::cli::Confirmation::Confirmed };
    auto out = std::ostringstream {};
    auto err = std::ostringstream {};
    CHECK(contour::cli::daemonStopVerb(
              port,
              "/run/c/default",
              { .consent = contour::cli::StopConsent::Given, .savedWorkspace = &store },
              prompt,
              out,
              err)
          == contour::cli::ExitStatus::Unreachable);
    CHECK_FALSE(saved.files.exists(store.path()));
    CHECK(prompt.asked.empty());
    CHECK(err.str() == "no daemon is running; it left no workspace to clear\n");
}

TEST_CASE("daemon stop --keep-snapshot with no daemon running keeps it", "[contour][cli][daemon]")
{
    auto saved = SavedWorkspace {};
    auto const before = saved.files.readFile("/state/contour/sessions/default.snapshot");
    auto port = FakeControlPort { ControlErrorCode::NotRunning };
    auto prompt = FakePrompt { contour::cli::Confirmation::Confirmed };
    auto out = std::ostringstream {};
    auto err = std::ostringstream {};
    CHECK(contour::cli::daemonStopVerb(
              port,
              "/run/c/default",
              { .disposition = contour::cli::StopDisposition::KeepSnapshot, .savedWorkspace = &saved.store },
              prompt,
              out,
              err)
          == contour::cli::ExitStatus::Unreachable);
    CHECK(saved.files.readFile("/state/contour/sessions/default.snapshot") == before);
    CHECK(saved.store.load().verdict == vthost::LoadVerdict::Restorable);
    CHECK(err.str() == "no daemon is running on /run/c/default\n");
}

TEST_CASE("daemon stop --keep-snapshot asks the daemon to keep the workspace", "[contour][cli][daemon]")
{
    auto status = sampleStatus();
    status.capabilities = vthost::proto::CapabilitySet::of(
        { vthost::proto::Capability::DaemonControl, vthost::proto::Capability::WorkspaceRestore });
    auto port = FakeControlPort { status };
    auto prompt = FakePrompt { contour::cli::Confirmation::Confirmed };
    auto out = std::ostringstream {};
    auto err = std::ostringstream {};
    auto const keep =
        contour::cli::StopOptions { .consent = contour::cli::StopConsent::Given,
                                    .disposition = contour::cli::StopDisposition::KeepSnapshot };

    SECTION("--keep-snapshot")
    {
        CHECK(contour::cli::daemonStopVerb(port, "/run/c/default", keep, prompt, out, err)
              == contour::cli::ExitStatus::Ok);
        CHECK(port.stops == std::vector { vthost::proto::StopReason::UserKeepingWorkspace });
        CHECK(out.str() == "daemon stopped; its workspace is kept\n");
    }
    SECTION("a plain stop")
    {
        CHECK(contour::cli::daemonStopVerb(
                  port, "/run/c/default", { .consent = contour::cli::StopConsent::Given }, prompt, out, err)
              == contour::cli::ExitStatus::Ok);
        CHECK(port.stops == std::vector { vthost::proto::StopReason::User });
    }
    SECTION("a daemon that keeps no workspace is stopped plainly")
    {
        auto older = FakeControlPort { sampleStatus() };
        CHECK(contour::cli::daemonStopVerb(older, "/run/c/default", keep, prompt, out, err)
              == contour::cli::ExitStatus::Ok);
        CHECK(older.stops == std::vector { vthost::proto::StopReason::User });
        CHECK(err.str() == "contour daemon: this daemon keeps no workspace\n");
        CHECK(out.str() == "daemon stopped\n");
    }
}

TEST_CASE("daemon.restore becomes the daemon's persistence", "[contour][cli][daemon]")
{
    using vthost::ScrollbackPersistence;
    auto restore = contour::config::DaemonRestoreConfig {};
    restore.rerunPrograms = { "vim" };
    restore.interval = std::chrono::seconds { 60 };
    auto const kept = contour::cli::workspacePersistenceOf(restore, "/s/default.snapshot");
    CHECK(kept.snapshotPath == std::filesystem::path { "/s/default.snapshot" });
    CHECK(kept.scrollback == ScrollbackPersistence::Tail);
    CHECK(kept.scrollbackRows == 1000);
    CHECK(kept.rerunPrograms == std::vector<std::string> { "vim" });
    CHECK(kept.interval == std::chrono::seconds { 60 });

    restore.scrollback = false;
    CHECK(contour::cli::workspacePersistenceOf(restore, {}).scrollback == ScrollbackPersistence::Off);
    restore.scrollback = true;
    restore.scrollbackLines = 0;
    CHECK(contour::cli::workspacePersistenceOf(restore, {}).scrollback == ScrollbackPersistence::Off);
}

TEST_CASE("a log follower prints what was appended and survives truncation", "[contour][cli][daemon]")
{
    auto files = core::platform::testing::InMemoryFileSystem {};
    std::ignore = files.writeFile("/l.log", "a\nb\nc\n");
    auto follower = contour::cli::LogFollower { files, "/l.log" };
    CHECK(follower.tail(2) == "b\nc\n");
    std::ignore = files.appendFile("/l.log", "d\n");
    CHECK(follower.poll() == "d\n");
    CHECK(follower.poll().empty());
    std::ignore = files.writeFile("/l.log", "x\n");
    CHECK(follower.poll() == "x\n");
}

TEST_CASE("a log follower's tail of 0 lines is the whole file", "[contour][cli][daemon]")
{
    auto files = core::platform::testing::InMemoryFileSystem {};
    std::ignore = files.writeFile("/l.log", "a\nb\nc\n");
    auto follower = contour::cli::LogFollower { files, "/l.log" };
    CHECK(follower.tail(0) == "a\nb\nc\n");
}

TEST_CASE("logs prints the running daemon's log, following until told to stop", "[contour][cli][daemon]")
{
    auto files = core::platform::testing::InMemoryFileSystem {};
    std::ignore = files.writeFile("/l.log", "one\ntwo\n");
    auto port = FakeControlPort { sampleStatus() };
    auto out = std::ostringstream {};
    auto err = std::ostringstream {};

    SECTION("once")
    {
        auto const tick =
            std::function<contour::cli::FollowStep()> { [] { return contour::cli::FollowStep::Stop; } };
        CHECK(contour::cli::daemonLogsVerb(port, "/fallback.log", {}, files, tick, out, err)
              == contour::cli::ExitStatus::Ok);
        CHECK(out.str() == "one\ntwo\n");
    }
    SECTION("follow")
    {
        auto ticks = 0;
        auto const tick = std::function<contour::cli::FollowStep()> { [&] {
            if (++ticks == 1)
            {
                std::ignore = files.appendFile("/l.log", "three\n");
                return contour::cli::FollowStep::Continue;
            }
            return contour::cli::FollowStep::Stop;
        } };
        auto const options =
            contour::cli::LogsOptions { .lines = 1, .follow = contour::cli::FollowMode::Follow };
        CHECK(contour::cli::daemonLogsVerb(port, "/fallback.log", options, files, tick, out, err)
              == contour::cli::ExitStatus::Ok);
        CHECK(out.str() == "two\nthree\n");
    }
}

TEST_CASE("logs of an absent daemon reads the fallback file, noting it", "[contour][cli][daemon]")
{
    auto files = core::platform::testing::InMemoryFileSystem {};
    std::ignore = files.writeFile("/fallback.log", "old\n");
    auto port = FakeControlPort { ControlErrorCode::NotRunning };
    auto out = std::ostringstream {};
    auto err = std::ostringstream {};
    auto const tick =
        std::function<contour::cli::FollowStep()> { [] { return contour::cli::FollowStep::Stop; } };
    CHECK(contour::cli::daemonLogsVerb(port, "/fallback.log", {}, files, tick, out, err)
          == contour::cli::ExitStatus::Ok);
    CHECK(out.str() == "old\n");
    CHECK(err.str().contains("/fallback.log"));
}

TEST_CASE("logs of a daemon logging to standard error says so", "[contour][cli][daemon]")
{
    auto files = core::platform::testing::InMemoryFileSystem {};
    auto status = sampleStatus();
    status.logFile.clear();
    auto port = FakeControlPort { status };
    auto out = std::ostringstream {};
    auto err = std::ostringstream {};
    auto const tick =
        std::function<contour::cli::FollowStep()> { [] { return contour::cli::FollowStep::Stop; } };
    CHECK(contour::cli::daemonLogsVerb(port, "/fallback.log", {}, files, tick, out, err)
          == contour::cli::ExitStatus::Failed);
    CHECK(err.str().contains("the daemon logs to its standard error"));
}

TEST_CASE("options given before and after a sub-verb merge", "[contour][cli][daemon]")
{
    using contour::cli::mergeSharedOption;
    CHECK(mergeSharedOption("socket", "", "", "") == "");
    CHECK(mergeSharedOption("socket", "X", "", "") == "X");
    CHECK(mergeSharedOption("socket", "", "X", "") == "X");
    CHECK(mergeSharedOption("socket", "X", "X", "") == "X");
    auto const clash = mergeSharedOption("socket", "X", "Y", "");
    REQUIRE(!clash);
    CHECK(clash.error().contains("--socket"));
}

TEST_CASE("daemon upgrade with no daemon exits 4", "[contour][cli][daemon]")
{
    auto port = UpgradablePort {};
    port.notRunning = true;
    auto binary = FakeBinarySource {};
    auto pacer = ManualPacer {};
    auto out = std::ostringstream {};
    auto err = std::ostringstream {};
    CHECK(contour::cli::daemonUpgradeVerb(port, binary, pacer, "2.0.0", "/run/c/default", out, err)
          == contour::cli::ExitStatus::Unreachable);
    CHECK(contour::cli::exitCode(contour::cli::ExitStatus::Unreachable) == 4);
    CHECK(err.str() == "no daemon is running on /run/c/default\n");
}

TEST_CASE("daemon upgrade exits 0 once the daemon upgrades", "[contour][cli][daemon]")
{
    auto port = UpgradablePort {};
    auto binary = FakeBinarySource {};
    auto pacer = ManualPacer {};
    auto out = std::ostringstream {};
    auto err = std::ostringstream {};
    CHECK(contour::cli::daemonUpgradeVerb(port, binary, pacer, "2.0.0", "/run/c/default", out, err)
          == contour::cli::ExitStatus::Ok);
    CHECK(out.str().contains("2.0.0"));
    CHECK(err.str().empty());
}

TEST_CASE("daemon upgrade exits 5 with the refusal text", "[contour][cli][daemon]")
{
    auto port = UpgradablePort {};
    port.reply = vthost::proto::UpgradeDaemonReply { .result = 3, .detail = {} }; // Sandboxed
    auto binary = FakeBinarySource {};
    auto pacer = ManualPacer {};
    auto out = std::ostringstream {};
    auto err = std::ostringstream {};
    CHECK(contour::cli::daemonUpgradeVerb(port, binary, pacer, "2.0.0", "/run/c/default", out, err)
          == contour::cli::ExitStatus::Refused);
    CHECK(contour::cli::exitCode(contour::cli::ExitStatus::Refused) == 5);
    CHECK(err.str().contains(vthost::handover::describe(vthost::handover::UpgradeRefusal::Sandboxed)));
}

TEST_CASE("daemon upgrade settles an unanswered request by asking the release again",
          "[contour][cli][daemon]")
{
    auto port = UpgradablePort {};
    auto binary = FakeBinarySource {};
    auto pacer = ManualPacer {};
    auto out = std::ostringstream {};
    auto err = std::ostringstream {};
    port.error = ControlError { .code = ControlErrorCode::Timeout, .detail = "no answer" };
    SECTION("it now runs another release")
    {
        port.releaseAfterError = "2.0.0";
        CHECK(contour::cli::daemonUpgradeVerb(port, binary, pacer, "2.0.0", "/run/c/default", out, err)
              == contour::cli::ExitStatus::Ok);
    }
    SECTION("it never comes back: the outcome is unknown, which is a failure and no refusal")
    {
        port.absentAfterError = true;
        CHECK(contour::cli::daemonUpgradeVerb(port, binary, pacer, "2.0.0", "/run/c/default", out, err)
              == contour::cli::ExitStatus::Failed);
        CHECK(err.str().contains("unknown"));
    }
    SECTION("it is back on the old release: the daemon declined")
    {
        CHECK(contour::cli::daemonUpgradeVerb(port, binary, pacer, "2.0.0", "/run/c/default", out, err)
              == contour::cli::ExitStatus::Refused);
    }
}

TEST_CASE("daemon upgrade exits 5 when the daemon refuses after it started", "[contour][cli][daemon]")
{
    // The daemon answered "started", disconnected every client, then could not quiesce and rolled back: the
    // status it serves again with is where the refusal is read.
    using vthost::handover::UpgradeRefusal;
    auto port = UpgradablePort {};
    port.upgradedRelease = "1.0.0";
    port.record = vthost::proto::DaemonUpgradeRecord { .generation = 2 };
    port.recordAfterUpgrade = vthost::proto::DaemonUpgradeRecord {
        .generation = 2,
        .attempts = 1,
        .lastFailure = static_cast<uint8_t>(1 + std::to_underlying(UpgradeRefusal::QuiesceTimedOut)),
        .lastFailureDetail = std::string { vthost::handover::describe(UpgradeRefusal::QuiesceTimedOut) }
    };
    auto binary = FakeBinarySource {};
    auto pacer = ManualPacer {};
    auto out = std::ostringstream {};
    auto err = std::ostringstream {};
    CHECK(contour::cli::daemonUpgradeVerb(port, binary, pacer, "1.0.0", "/run/c/default", out, err)
          == contour::cli::ExitStatus::Refused);
    CHECK(out.str().empty());
    CHECK(err.str().contains(vthost::handover::describe(UpgradeRefusal::QuiesceTimedOut)));
}

TEST_CASE("daemon upgrade to the same release succeeds by the generation", "[contour][cli][daemon]")
{
    auto port = UpgradablePort {};
    port.upgradedRelease = "1.0.0";
    port.record = vthost::proto::DaemonUpgradeRecord { .generation = 0 };
    port.recordAfterUpgrade = vthost::proto::DaemonUpgradeRecord { .generation = 1 };
    auto binary = FakeBinarySource {};
    auto pacer = ManualPacer {};
    auto out = std::ostringstream {};
    auto err = std::ostringstream {};
    CHECK(contour::cli::daemonUpgradeVerb(port, binary, pacer, "1.0.0", "/run/c/default", out, err)
          == contour::cli::ExitStatus::Ok);
    CHECK(err.str().empty());
}

TEST_CASE("daemon upgrade names the release the daemon serves, not the one it was asked for",
          "[contour][cli][daemon]")
{
    auto port = UpgradablePort {};
    port.upgradedRelease = "3.0.0";
    port.record = vthost::proto::DaemonUpgradeRecord { .generation = 0 };
    port.recordAfterUpgrade = vthost::proto::DaemonUpgradeRecord { .generation = 1, .attempts = 1 };
    auto binary = FakeBinarySource {};
    auto pacer = ManualPacer {};
    auto out = std::ostringstream {};
    auto err = std::ostringstream {};
    // The record makes the CLI settle the upgrade, and the settling status names what runs.
    CHECK(contour::cli::daemonUpgradeVerb(port, binary, pacer, "2.0.0", "/run/c/default", out, err)
          == contour::cli::ExitStatus::Ok);
    CHECK(out.str().contains("3.0.0"));
    CHECK_FALSE(out.str().contains("2.0.0"));
}

TEST_CASE("status names the upgrade generation and the last failed upgrade", "[contour][cli][daemon]")
{
    auto status = sampleStatus();
    CHECK_FALSE(contour::cli::formatDaemonStatus(status).contains("upgrades:"));
    status.upgrade = vthost::proto::DaemonUpgradeRecord { .generation = 2 };
    auto const text = contour::cli::formatDaemonStatus(status);
    CHECK(text.contains("upgrades:"));
    CHECK(text.contains(" 2\n"));
    CHECK_FALSE(text.contains("last upgrade"));
    status.upgrade->lastFailure = vthost::proto::UpgradeDaemonReply::ExecFailedResult;
    status.upgrade->lastFailureDetail = "Exec format error";
    CHECK(contour::cli::formatDaemonStatus(status).contains(
        "execFailed: " + std::string { vthost::handover::ExecFailureText }));
}

TEST_CASE("a daemon service is registered with the running executable, not with argv[0]",
          "[contour][cli][daemon]")
{
    auto const installed = std::filesystem::path { "/opt/contour/bin/contour" };
    // Started through the PATH, argv[0] is the bare name: absolute() of it would be a file in the cwd.
    CHECK(contour::cli::daemonServiceProgram(installed, "contour") == installed);
    CHECK(contour::cli::daemonServiceProgram(installed, "./relative/contour") == installed);
}

TEST_CASE("a daemon service falls back to argv[0] made absolute when the executable is not known",
          "[contour][cli][daemon]")
{
    auto const fallback = contour::cli::daemonServiceProgram(
        std::unexpected(std::make_error_code(std::errc::no_such_file_or_directory)), "contour");
    CHECK(fallback.is_absolute());
    CHECK(fallback.filename() == "contour");
}
