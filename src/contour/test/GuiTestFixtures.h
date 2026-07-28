// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/CommandCatalog.h>
#include <contour/CommandHistoryStore.h>
#include <contour/ContourGuiApp.h>
#include <contour/ExternalLauncher.h>
#include <contour/LayoutStore.h>
#include <contour/SessionFactory.h>
#include <contour/TerminalSessionManager.h>
#include <contour/WindowController.h>

#include <vtpty/MockPty.h>
#include <vtpty/Process.h>
#include <vtpty/Pty.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QEvent>
#include <QtCore/QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <expected>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <vtworkspace/Primitives.h>

namespace contour::test
{

/// In-memory SessionFactory: every created session is backed by a vtpty::MockPty, so the manager's
/// session-creation paths (new tab, new split pane) run headlessly — no process is spawned, and a
/// test can seed/inspect each PTY's buffers. Records the cwd handed to each creation so
/// working-directory inheritance is assertable.
class MockPtySessionFactory final: public contour::SessionFactory
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
[[nodiscard]] inline vtpty::MockPty& mockPtyOf(contour::TerminalSession& session)
{
    return dynamic_cast<vtpty::MockPty&>(session.terminal().device());
}

/// Loads @p yaml through the PRODUCTION config file loader (writing it to a throwaway temp file
/// first), so a test asserts what a real user's configuration would parse to — including the
/// sibling-layouts merge and every fallback loadConfigFromFile applies.
/// @param yaml The inline configuration document.
/// @return The parsed configuration.
[[nodiscard]] inline contour::config::Config loadConfigFromYaml(std::string_view yaml)
{
    QTemporaryDir const dir;
    REQUIRE(dir.isValid());
    auto const path = std::filesystem::path(dir.path().toStdString()) / "contour.yml";
    {
        auto out = std::ofstream(path);
        out << yaml;
    }
    auto config = contour::config::Config {};
    contour::config::loadConfigFromFile(config, path);
    return config;
}

/// An in-memory LayoutStore: SaveLayout runs end to end (serialize -> persist -> re-read) with no
/// filesystem at all, and a test can inspect exactly what was handed to persistence. @c loadError
/// makes the store report an unreadable backing file, to exercise the refuse-rather-than-destroy
/// path without corrupting a real one.
class InMemoryLayoutStore final: public contour::LayoutStore
{
  public:
    [[nodiscard]] std::expected<contour::LayoutMap, std::string> load(
        std::filesystem::path const& path) const override
    {
        loadedPaths.push_back(path);
        if (loadError)
            return std::unexpected(*loadError);
        return layouts;
    }

    [[nodiscard]] std::expected<void, std::string> save(std::filesystem::path const& path,
                                                        contour::LayoutMap const& newLayouts) override
    {
        savedPaths.push_back(path);
        if (saveError)
            return std::unexpected(*saveError);
        layouts = newLayouts;
        return {};
    }

    /// The store's contents (seed it to model a pre-existing layouts.yml; read it back to assert
    /// what SaveLayout persisted).
    contour::LayoutMap layouts;
    /// When set, load() fails with this message (an unreadable/corrupt backing file).
    std::optional<std::string> loadError;
    /// When set, save() fails with this message (permissions, disk full, ...).
    std::optional<std::string> saveError;
    /// The path each load()/save() was asked for, so a test can assert WHERE layouts persist.
    mutable std::vector<std::filesystem::path> loadedPaths;
    std::vector<std::filesystem::path> savedPaths;
};

/// An in-memory CommandHistoryStore: the command palette's record -> persist -> reload cycle runs end
/// to end with no filesystem at all. Mirrors InMemoryLayoutStore, including the injectable failures,
/// so a test can drive the "the history file is corrupt" path without corrupting a real one.
class InMemoryCommandHistoryStore final: public contour::CommandHistoryStore
{
  public:
    [[nodiscard]] std::expected<std::vector<std::string>, std::string> load(
        std::filesystem::path const& path) const override
    {
        loadedPaths.push_back(path);
        if (loadError)
            return std::unexpected(*loadError);
        return ids;
    }

    [[nodiscard]] std::expected<void, std::string> save(std::filesystem::path const& path,
                                                        std::span<std::string const> newIds) override
    {
        savedPaths.push_back(path);
        if (saveError)
            return std::unexpected(*saveError);
        ids.assign(newIds.begin(), newIds.end());
        return {};
    }

    /// The store's contents, newest first (seed it to model a pre-existing command-history.yml; read
    /// it back to assert what the palette persisted).
    std::vector<std::string> ids;
    /// When set, load() fails with this message (an unreadable/corrupt backing file).
    std::optional<std::string> loadError;
    /// When set, save() fails with this message (permissions, disk full, ...).
    std::optional<std::string> saveError;
    /// The path each load()/save() was asked for, so a test can assert WHERE the history persists.
    mutable std::vector<std::filesystem::path> loadedPaths;
    std::vector<std::filesystem::path> savedPaths;
};

/// A TabTitleProvider over a fixed list of titles, so the command palette's tab source can be driven —
/// and its rows asserted — without a window, an event loop, or a live session behind it.
class StubTabs final: public contour::TabTitleProvider
{
  public:
    explicit StubTabs(std::vector<std::string> titles): _titles { std::move(titles) } {}

    [[nodiscard]] std::vector<std::string> tabTitles() const override { return _titles; }

    /// Models tabs opening and closing under a palette that is already showing them.
    void setTitles(std::vector<std::string> titles) { _titles = std::move(titles); }

  private:
    std::vector<std::string> _titles;
};

/// Records every URL-open / process-spawn request instead of launching it, so tests can assert the
/// routing and validation of the open-document, follow-hyperlink, open-configuration/-file-manager
/// /-selection, and spawn-new-terminal actions without touching the desktop.
class RecordingExternalLauncher final: public contour::ExternalLauncher
{
  public:
    struct Execution
    {
        QString program;
        QStringList arguments;
    };

    [[nodiscard]] bool openUrl(QUrl const& url) override
    {
        openedUrls.push_back(url);
        return openUrlResult;
    }

    bool runDetached(QString const& program, QStringList const& arguments) override
    {
        detached.push_back({ program, arguments });
        return true;
    }

    int execute(QString const& program, QStringList const& arguments) override
    {
        executed.push_back({ program, arguments });
        return 0;
    }

    std::vector<QUrl> openedUrls;
    std::vector<Execution> detached;
    std::vector<Execution> executed;
    /// The value openUrl() returns (flip to false to exercise the "could not open" error path).
    bool openUrlResult = true;
};

/// A SpeechSynthesizer that records what it was asked to say instead of saying it.
///
/// The read-aloud path cannot otherwise be driven headlessly: the real synthesizer answers
/// available() only by connecting to the machine's speech service and enumerating its voices, so a
/// test would depend on the developer's installed voices and would speak out loud when they have one.
class RecordingSpeechSynthesizer final: public contour::SpeechSynthesizer
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

/// In-memory PTY with REAL blocking-read semantics, for a test that runs a LIVE session read loop
/// (the render harness, where TerminalDisplay::setSession starts the session's threads).
///
/// vtpty::MockPty returns a zero-length chunk on an empty buffer, which the terminal loop treats as
/// EOF (processInputOnce closes the PTY) — correct for synchronous parse-what-you-seeded tests, fatal
/// for a session that must stay alive between feeds. This mock mirrors UnixPty instead: an empty
/// buffer BLOCKS the reader (honoring the read timeout) until data arrives, a wakeupReader(), or
/// close(); EOF (empty read) is reported only once closed. wakeupReader() genuinely unblocks the
/// reader (as UnixPty's break-pipe does), so ~TerminalSession's thread join completes.
/// Not final: ConfigurableStartPty derives from it to vary start() alone, the blocking read being
/// exactly what a session that must survive its own start() needs.
class BlockingMockPty: public vtpty::Pty
{
  public:
    explicit BlockingMockPty(vtbackend::PageSize windowSize): _pageSize { windowSize } {}

    vtpty::PtySlave& slave() noexcept override { return _slave; }

    [[nodiscard]] std::optional<ReadResult> read(crispy::buffer_object<char>& storage,
                                                 std::optional<std::chrono::milliseconds> timeout,
                                                 size_t size) override
    {
        auto lock = std::unique_lock { _mutex };
        auto const wake = [this]() {
            return _closed || _woken || _outputReadOffset < _outputBuffer.size();
        };
        if (timeout.has_value())
            _wakeup.wait_for(lock, *timeout, wake);
        else
            _wakeup.wait(lock, wake);

        if (_outputReadOffset == _outputBuffer.size())
        {
            if (_closed)
                // Drained and closed: report EOF (empty read), as a real PTY does.
                return ReadResult { .data = std::string_view {}, .fromStdoutFastPipe = false };
            // A bare wakeupReader() (teardown wake, no data) or a genuine timeout: return EAGAIN so
            // the caller re-checks its _terminating flag, exactly as UnixPty's break-pipe wake does.
            _woken = false;
            errno = EAGAIN;
            return std::nullopt;
        }

        auto const n = std::min({ size, _outputBuffer.size() - _outputReadOffset, storage.bytesAvailable() });
        auto const chunk = std::string_view { _outputBuffer.data() + _outputReadOffset, n };
        _outputReadOffset += n;
        auto const pooled = storage.writeAtEnd(chunk);
        return ReadResult { .data = std::string_view(pooled.data(), pooled.size()),
                            .fromStdoutFastPipe = false };
    }

    void wakeupReader() override
    {
        {
            auto const lock = std::lock_guard { _mutex };
            _woken = true;
        }
        _wakeup.notify_all();
    }

    int write(std::string_view data) override
    {
        auto const lock = std::lock_guard { _mutex };
        _inputBuffer.append(data);
        return static_cast<int>(data.size());
    }

    [[nodiscard]] vtbackend::PageSize pageSize() const noexcept override { return _pageSize; }
    void resizeScreen(vtbackend::PageSize cells, std::optional<vtpty::ImageSize> pixels) override
    {
        auto const lock = std::lock_guard { _mutex };
        if (_closed)
            ++_resizesAfterClose;
        _pageSize = cells;
        _pixelSize = pixels;
    }

    /// How many resizes arrived after close(). A real PTY has no device left to resize by then --
    /// ConPty crashed outright on one (it handed the invalidated HPCON to ResizePseudoConsole) --
    /// so this must stay zero.
    [[nodiscard]] int resizesAfterClose() const noexcept
    {
        auto const lock = std::lock_guard { _mutex };
        return _resizesAfterClose;
    }

    vtpty::StartResult start() override { return {}; }
    void close() override
    {
        {
            auto const lock = std::lock_guard { _mutex };
            _closed = true;
        }
        _wakeup.notify_all();
    }
    void waitForClosed() override
    {
        auto lock = std::unique_lock { _mutex };
        _wakeup.wait(lock, [this]() { return _closed; });
    }
    [[nodiscard]] bool isClosed() const noexcept override
    {
        auto const lock = std::lock_guard { _mutex };
        return _closed;
    }

    /// Feeds VT output to the (possibly blocked) session read loop.
    void feed(std::string_view data)
    {
        {
            auto const lock = std::lock_guard { _mutex };
            if (_outputReadOffset == _outputBuffer.size())
            {
                _outputReadOffset = 0;
                _outputBuffer.assign(data);
            }
            else
                _outputBuffer.append(data);
        }
        _wakeup.notify_all();
    }

    /// Snapshot of everything the terminal wrote into the PTY (keyboard/mouse encodings, replies).
    [[nodiscard]] std::string stdinSnapshot() const
    {
        auto const lock = std::lock_guard { _mutex };
        return _inputBuffer;
    }

    /// Whether fed output is still waiting to be consumed by the session's read loop.
    [[nodiscard]] bool isStdoutPending() const
    {
        auto const lock = std::lock_guard { _mutex };
        return _outputReadOffset < _outputBuffer.size();
    }

  private:
    mutable std::mutex _mutex;
    std::condition_variable _wakeup;
    vtbackend::PageSize _pageSize;
    std::optional<vtpty::ImageSize> _pixelSize;
    std::string _inputBuffer;
    std::string _outputBuffer;
    std::size_t _outputReadOffset = 0;
    int _resizesAfterClose = 0; ///< Resizes that arrived once _closed; see resizesAfterClose().
    bool _closed = false;
    bool _woken = false; ///< One-shot wakeupReader() flag (teardown wake), cleared on read.
    vtpty::PtySlaveDummy _slave;
};

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
/// Built on BlockingMockPty rather than vtpty::MockPty because the Diagnose case must leave a session
/// that is genuinely UP: MockPty answers an empty buffer with a zero-length read, which
/// Terminal::processInputOnce() reads as EOF — so the read loop this start() has just launched would
/// close the device from under the test within microseconds of returning. That is a race a debug build
/// happens to win and a release build loses.
class ConfigurableStartPty final: public BlockingMockPty
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
        BlockingMockPty { pageSize }, _behavior { behavior }
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
                return BlockingMockPty::start().transform([](vtpty::StartOutcome outcome) {
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
    explicit TestApp(std::unique_ptr<contour::SessionFactory> factory = nullptr,
                     std::unique_ptr<contour::LayoutStore> layoutStore = nullptr,
                     std::unique_ptr<contour::CommandHistoryStore> commandHistoryStore = nullptr,
                     std::unique_ptr<contour::SpeechSynthesizer> speech = nullptr):
        _app(std::move(factory),
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
                     std::unique_ptr<contour::SessionFactory> factory = nullptr):
        _app(std::move(factory), makeRecordingLauncher(), nullptr, nullptr, defaultSpeech())
    {
        std::vector<char const*> argv;
        argv.reserve(args.size() + 1);
        argv.push_back("contour");
        for (auto const* a: args)
            argv.push_back(a);
        REQUIRE(_app.parseParametersForTesting(static_cast<int>(argv.size()), argv.data()));
    }

    [[nodiscard]] contour::ContourGuiApp& app() noexcept { return _app; }
    [[nodiscard]] contour::TerminalSessionManager& manager() noexcept { return _app.sessionsManager(); }
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
    [[nodiscard]] static std::unique_ptr<contour::SpeechSynthesizer> defaultSpeech()
    {
        return std::make_unique<contour::NullSpeechSynthesizer>();
    }

    /// Builds the recording launcher, stashing a raw observation pointer before handing ownership to
    /// the app. Runs in the member-initializer list, so _launcher is set before _app is constructed.
    std::unique_ptr<contour::ExternalLauncher> makeRecordingLauncher()
    {
        auto launcher = std::make_unique<RecordingExternalLauncher>();
        _launcher = launcher.get();
        return launcher;
    }

    RecordingExternalLauncher* _launcher = nullptr;
    contour::ContourGuiApp _app;
};

/// Owns one manager-minted WindowController for a test's lifetime. Production deletes controllers
/// through removeWindowController() (which deleteLater()s them); the guard replays that and drains
/// the deferred-delete queue so LeakSanitizer sees a clean teardown. Removal is idempotent, so a
/// test may also remove the controller explicitly.
struct ScopedController
{
    contour::TerminalSessionManager& manager;
    contour::WindowController* controller;
    vtworkspace::WindowId id;

    explicit ScopedController(contour::TerminalSessionManager& m):
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

    [[nodiscard]] contour::WindowController* operator->() const noexcept { return controller; }
    [[nodiscard]] contour::WindowController& operator*() const noexcept { return *controller; }
};

} // namespace contour::test
