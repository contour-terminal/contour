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

#include <expected>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <vtmux/Primitives.h>

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

// The former fixture-local BlockingMockPty (an in-memory PTY with real blocking-read semantics
// for tests running a LIVE session read loop) now ships as vtpty::ChannelPty, so production
// remote sessions run on the identical Pty.

/// Builds a ContourGuiApp whose parameters() are populated with defaults (so profileName() resolves
/// to the default "main" profile) without running the GUI. The default-constructed config already
/// seeds a "main" profile and a "default" colorscheme, so no config file is needed. The app's real
/// session manager is usable headlessly as long as no PTY is spawned (tabs minted straight through
/// the vtmux model have no backing sessions).
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
    explicit TestApp(std::unique_ptr<contour::SessionFactory> factory = nullptr,
                     std::unique_ptr<contour::LayoutStore> layoutStore = nullptr,
                     std::unique_ptr<contour::CommandHistoryStore> commandHistoryStore = nullptr):
        _app(std::move(factory),
             makeRecordingLauncher(),
             std::move(layoutStore),
             std::move(commandHistoryStore))
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
        _app(std::move(factory), makeRecordingLauncher())
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
    vtmux::WindowId id;

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
