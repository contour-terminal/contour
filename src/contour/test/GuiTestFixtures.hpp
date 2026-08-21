// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/ContourGuiApp.hpp>
#include <contour/command/CommandCatalog.hpp>
#include <contour/command/CommandHistoryStore.hpp>
#include <contour/config/LayoutStore.hpp>
#include <contour/platform/ExternalLauncher.hpp>
#include <contour/session/SessionFactory.hpp>
#include <contour/session/TerminalSessionManager.hpp>
#include <contour/test/CoreFixtures.hpp>
#include <contour/test/LauncherFixtures.hpp>
#include <contour/window/WindowController.hpp>

#include <vtpty/ChannelPty.hpp>
#include <vtpty/MockPty.hpp>
#include <vtpty/Process.hpp>
#include <vtpty/Pty.hpp>

#include <crispy/Environment.hpp>

#include <QtCore/QCoreApplication>
#include <QtCore/QEvent>
#include <QtCore/QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

#include <expected>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <vtworkspace/Primitives.hpp>

namespace contour::test
{

/// In-memory SessionFactory: every created session is backed by a vtpty::MockPty, so the manager's
/// session-creation paths (new tab, new split pane) run headlessly — no process is spawned, and a
/// test can seed/inspect each PTY's buffers. Records the cwd handed to each creation so
/// working-directory inheritance is assertable.
class MockPtySessionFactory final: public contour::session::SessionFactory
{
  public:
    /// The page size a created PTY falls back to when the caller passes no explicit @c pageSize (a
    /// brand-new window). Mirrors the profile default a real AppSessionFactory would use.
    static constexpr auto DefaultPageSize =
        vtbackend::PageSize { vtbackend::LineCount(25), vtbackend::ColumnCount(80) };

    [[nodiscard]] std::unique_ptr<vtpty::Pty> createPty(
        std::optional<std::string> cwd,
        std::optional<vtbackend::PageSize> pageSize = std::nullopt,
        std::optional<vtpty::Process::ExecInfo> commandOverride = std::nullopt,
        std::optional<std::string> profileName = std::nullopt) override
    {
        requestedCwds.push_back(std::move(cwd));
        // Record the size the manager asked for (nullopt == "use the profile default"), so a test can
        // assert that a new tab/split inherited the running window size instead of the default.
        requestedPageSizes.push_back(pageSize);
        requestedCommandOverrides.push_back(std::move(commandOverride));
        requestedProfileNames.push_back(std::move(profileName));
        auto const initialSize = pageSize.value_or(DefaultPageSize);
        auto pty = std::make_unique<vtpty::MockPty>(initialSize);
        createdPtys.push_back(pty.get());
        return pty;
    }

    std::vector<std::optional<std::string>> requestedCwds;
    /// The @c pageSize argument of each createPty() call (nullopt when the caller requested the default).
    std::vector<std::optional<vtbackend::PageSize>> requestedPageSizes;
    /// The @c commandOverride argument of each createPty() call.
    std::vector<std::optional<vtpty::Process::ExecInfo>> requestedCommandOverrides;
    /// The @c profileName argument of each createPty() call.
    std::vector<std::optional<std::string>> requestedProfileNames;
    /// Non-owning observation pointers; valid while the owning session lives.
    std::vector<vtpty::MockPty*> createdPtys;
};

/// The MockPty backing @p session, for seeding output and inspecting the bytes the terminal wrote
/// towards the shell (key/mouse encodings, replies, focus events).
/// @param session A session created over a MockPty (via MockPtySessionFactory or directly).
/// @return The session's PTY, downcast. Throws std::bad_cast if it is not a MockPty.
[[nodiscard]] inline vtpty::MockPty& mockPtyOf(contour::session::TerminalSession& session)
{
    return dynamic_cast<vtpty::MockPty&>(session.terminal().device());
}

/// An in-memory LayoutStore: SaveLayout runs end to end (serialize -> persist -> re-read) with no
/// filesystem at all, and a test can inspect exactly what was handed to persistence. @c loadError
/// makes the store report an unreadable backing file, to exercise the refuse-rather-than-destroy
/// path without corrupting a real one.
class InMemoryLayoutStore final: public contour::config::LayoutStore
{
  public:
    [[nodiscard]] std::expected<contour::config::LayoutMap, std::string> load(
        std::filesystem::path const& path) const override
    {
        loadedPaths.push_back(path);
        if (loadError)
            return std::unexpected(*loadError);
        return layouts;
    }

    [[nodiscard]] std::expected<void, std::string> save(std::filesystem::path const& path,
                                                        contour::config::LayoutMap const& newLayouts) override
    {
        savedPaths.push_back(path);
        if (saveError)
            return std::unexpected(*saveError);
        layouts = newLayouts;
        return {};
    }

    /// The store's contents (seed it to model a pre-existing layouts.yml; read it back to assert
    /// what SaveLayout persisted).
    contour::config::LayoutMap layouts;
    /// When set, load() fails with this message (an unreadable/corrupt backing file).
    std::optional<std::string> loadError;
    /// When set, save() fails with this message (permissions, disk full, ...).
    std::optional<std::string> saveError;
    /// The path each load()/save() was asked for, so a test can assert WHERE layouts persist.
    mutable std::vector<std::filesystem::path> loadedPaths;
    std::vector<std::filesystem::path> savedPaths;
};

/// A SpeechSynthesizer that records what it was asked to say instead of saying it.
///
/// The read-aloud path cannot otherwise be driven headlessly: the real synthesizer answers
/// available() only by connecting to the machine's speech service and enumerating its voices, so a
/// test would depend on the developer's installed voices and would speak out loud when they have one.
class RecordingSpeechSynthesizer final: public contour::platform::SpeechSynthesizer
{
  public:
    [[nodiscard]] bool available() const override { return isAvailable; }
    void say(std::string_view text) override { spoken.emplace_back(text); }
    void stop() override { ++stopCount; }

    /// Everything say() was handed, in order — already passed through speakableText().
    std::vector<std::string> spoken;
    /// How many times stop() was asked for.
    size_t stopCount = 0;
    /// What available() reports (flip to false for the "no voice on this machine" path).
    bool isAvailable = true;
};

// The former fixture-local BlockingMockPty (an in-memory PTY with real blocking-read semantics
// for tests running a LIVE session read loop) now ships as vtpty::ChannelPty, so production
// remote sessions run on the identical Pty.

/// PTY whose start() produces a caller-chosen outcome, so every way a device can come up — or fail
/// to — is drivable from one fixture rather than a family of near-identical ones.
///
/// The Throw case is the shape of the #1711 crash: on Windows a CreateProcess() failure is
/// discovered in the PARENT (there is no fork), and the only channel Process::start() had was an
/// exception. Thrown from TerminalSession::start(), it unwound out of TerminalDisplay::setSession()
/// mid-mutation — past emit sessionChanged() — leaving a half-bound display and a session that,
/// never having started its exit watcher, no onClosed() could ever prune. It is kept as a case
/// because no device may take the process down that way, whatever it throws.
///
/// Built on vtpty::ChannelPty rather than vtpty::MockPty because the Diagnose case must leave a
/// session that is genuinely UP: MockPty answers an empty buffer with a zero-length read, which
/// Terminal::processInputOnce() reads as EOF — so the read loop this start() has just launched would
/// close the device from under the test within microseconds of returning. That is a race a debug build
/// happens to win and a release build loses. ChannelPty blocks instead, which is why production
/// remote sessions run on it too.
class ConfigurableStartPty final: public vtpty::ChannelPty
{
  public:
    /// What start() does.
    enum class StartBehavior : uint8_t
    {
        Diagnose, ///< Starts, reporting what it had to give up (a spawn-ladder rung below the first).
        Fail,     ///< Reports a failure as a value, the way vtpty does.
        Throw,    ///< Throws, the way the Windows spawn used to. See issue #1711.
    };

    /// The message the Windows reporters in #1711 actually saw.
    static constexpr auto FailureText = std::string_view { "The directory name is invalid." };

    /// A non-fatal notice, as the spawn ladder's directory-dropping rung produces.
    static constexpr auto DiagnosticText =
        std::string_view { "Failed to start in \"/gone\". Using the current directory instead." };

    ConfigurableStartPty(vtbackend::PageSize pageSize, StartBehavior behavior):
        vtpty::ChannelPty { pageSize }, _behavior { behavior }
    {
    }

    vtpty::StartResult start() override
    {
        switch (_behavior)
        {
            case StartBehavior::Throw: throw std::runtime_error(std::string { FailureText });
            case StartBehavior::Fail:
                return std::unexpected(vtpty::StartFailure { .error = vtpty::StartError::SpawnFailed,
                                                             .detail = std::string { FailureText } });
            case StartBehavior::Diagnose:
                return vtpty::ChannelPty::start().transform([](vtpty::StartOutcome outcome) {
                    outcome.diagnostic = std::string { DiagnosticText };
                    return outcome;
                });
        }
        return {};
    }

  private:
    StartBehavior _behavior;
};

/// Builds a ContourGuiApp whose parameters() are populated with defaults (so profileName() resolves
/// to the default "main" profile) without running the GUI. The default-constructed config already
/// seeds a "main" profile and a "default" colorscheme, so no config file is needed. The app's real
/// session manager is usable headlessly as long as no PTY is spawned (tabs minted straight through
/// the vtworkspace model have no backing sessions).
class TestApp
{
  public:
    /// @param factory Optional PTY factory override; pass a MockPtySessionFactory (keep a raw
    ///                observation pointer first) to run session-creation paths headlessly.
    /// @param layoutStore Optional layout-persistence override; pass an InMemoryLayoutStore (keep a
    ///                raw observation pointer first) to drive SaveLayout without touching the disk.
    /// @param commandHistoryStore Optional command-history override; pass an
    ///                InMemoryCommandHistoryStore (keep a raw observation pointer first) to drive the
    ///                command palette's MRU persistence without touching the disk.
    ///
    /// @param speech Optional speech override; pass a RecordingSpeechSynthesizer (keep a raw
    ///                observation pointer first) to drive the read-aloud path. @see defaultSpeech()
    ///                for what a test app gets without one.
    explicit TestApp(std::unique_ptr<contour::session::SessionFactory> factory = nullptr,
                     std::unique_ptr<contour::config::LayoutStore> layoutStore = nullptr,
                     std::unique_ptr<contour::command::CommandHistoryStore> commandHistoryStore = nullptr,
                     std::unique_ptr<contour::platform::SpeechSynthesizer> speech = nullptr):
        _app(_environment,
             std::move(factory),
             makeRecordingLauncher(),
             std::move(layoutStore),
             std::move(commandHistoryStore),
             speech ? std::move(speech) : defaultSpeech())
    {
        char const* argv[] = { "contour", "terminal" };
        // Parse the "terminal" subcommand so parameters() carries every contour.terminal.* default
        // (profile, dump-state-at-exit, ...) that TerminalSession reads during construction.
        REQUIRE(_app.parseParametersForTesting(2, argv));
    }

    /// Parses an explicit argv so a test can drive a non-"terminal" subcommand (e.g. font-locator,
    /// info, documentation) whose action reads its own contour.<cmd>.* parameters.
    /// @param args The command tokens after the program name (e.g. {"font-locator"}).
    explicit TestApp(std::initializer_list<char const*> args,
                     std::unique_ptr<contour::session::SessionFactory> factory = nullptr):
        _app(_environment, std::move(factory), makeRecordingLauncher(), nullptr, nullptr, defaultSpeech())
    {
        std::vector<char const*> argv;
        argv.reserve(args.size() + 1);
        argv.push_back("contour");
        for (auto const* a: args)
            argv.push_back(a);
        REQUIRE(_app.parseParametersForTesting(static_cast<int>(argv.size()), argv.data()));
    }

    [[nodiscard]] contour::ContourGuiApp& app() noexcept { return _app; }
    [[nodiscard]] contour::session::TerminalSessionManager& manager() noexcept
    {
        return _app.sessionsManager();
    }
    /// The recording launcher wired into this app (records URL-open / process-spawn requests).
    [[nodiscard]] RecordingExternalLauncher& launcher() noexcept { return *_launcher; }

  private:
    /// The synthesizer a test app gets when the test does not name one.
    ///
    /// The NULL one, never the production synthesizer -- and this is the single place that decides
    /// so, because it is an invariant of the suite rather than a default anyone should vary. A real
    /// synthesizer connects to the machine's speech service (speech-dispatcher on Linux) as soon as
    /// a session is asked for its context-menu state: surprising for a headless suite, audible if
    /// the machine has a voice, and a leak inside libspeechd on every connection.
    [[nodiscard]] static std::unique_ptr<contour::platform::SpeechSynthesizer> defaultSpeech()
    {
        return std::make_unique<contour::platform::NullSpeechSynthesizer>();
    }

    /// Builds the recording launcher, stashing a raw observation pointer before handing ownership to
    /// the app. Runs in the member-initializer list, so _launcher is set before _app is constructed.
    std::unique_ptr<contour::platform::ExternalLauncher> makeRecordingLauncher()
    {
        auto launcher = std::make_unique<RecordingExternalLauncher>();
        _launcher = launcher.get();
        return launcher;
    }

    RecordingExternalLauncher* _launcher = nullptr;
    /// The process's own environment: what the fixture exercises is the GUI, not what any variable
    /// reads as. A test that cares injects a crispy::testing::FakeEnvironment instead.
    crispy::LiveEnvironment _environment;
    contour::ContourGuiApp _app;
};

/// Owns one manager-minted WindowController for a test's lifetime. Production deletes controllers
/// through removeWindowController() (which deleteLater()s them); the guard replays that and drains
/// the deferred-delete queue so LeakSanitizer sees a clean teardown. Removal is idempotent, so a
/// test may also remove the controller explicitly.
struct ScopedController
{
    contour::session::TerminalSessionManager& manager;
    contour::window::WindowController* controller;
    vtworkspace::WindowId id;

    explicit ScopedController(contour::session::TerminalSessionManager& m):
        manager(m), controller(m.createWindowController()), id(controller->windowId())
    {
    }
    ~ScopedController()
    {
        manager.removeWindowController(id); // no-op if the test already removed it
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    ScopedController(ScopedController const&) = delete;
    ScopedController& operator=(ScopedController const&) = delete;
    ScopedController(ScopedController&&) = delete;
    ScopedController& operator=(ScopedController&&) = delete;

    [[nodiscard]] contour::window::WindowController* operator->() const noexcept { return controller; }
    [[nodiscard]] contour::window::WindowController& operator*() const noexcept { return *controller; }
};

} // namespace contour::test
