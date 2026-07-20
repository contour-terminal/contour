// SPDX-License-Identifier: Apache-2.0
#include <contour/Actions.h>
#include <contour/BlurBehind.h>
#include <contour/ContourGuiApp.h>
#include <contour/WindowController.h>
#include <contour/display/ContentScale.h>
#include <contour/display/ImeQueryRect.h>
#include <contour/display/RhiRenderer.h>
#include <contour/display/TerminalAccessible.h>
#include <contour/display/Announcer.h>
#include <contour/display/TerminalDisplay.h>
#include <contour/display/TerminalRenderNode.h>
#include <contour/helper.h>

#include <vtbackend/Color.h>
#include <vtbackend/Metrics.h>

#include <vtpty/Pty.h>

#include <crispy/App.h>
#include <crispy/logstore.h>
#include <crispy/utils.h>

#include <QtCore/QDebug>
#include <QtCore/QFileInfo>
#include <QtCore/QMetaEnum>
#include <QtCore/QMetaObject>
#include <QtCore/QProcess>
#include <QtCore/QRunnable>
#include <QtCore/QSemaphore>
#include <QtCore/QStandardPaths>
#include <QtCore/QTimer>
#include <QtGui/QClipboard>
#include <QtGui/QDesktopServices>
#include <QtGui/QGuiApplication>
#include <QtGui/QKeyEvent>
#include <QtGui/QOpenGLContext>
#include <QtGui/QScreen>
#include <QtNetwork/QHostInfo>
#include <QtQml/QQmlContext>
#include <QtQuick/QQuickWindow>

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <ranges>
#include <string_view>
#include <variant>
#include <vector>

#include <rhi/qrhi.h>

namespace fs = std::filesystem;

#if defined(_MSC_VER)
    #define __PRETTY_FUNCTION__ __FUNCDNAME__
#endif

// Must be in global namespace
// NB: must be publicly visible, and due to -Wmissing-declarations, we better tell the compiler.
void initializeDisplayResources();

void initializeDisplayResources()
{
    Q_INIT_RESOURCE(DisplayResources);
}

namespace contour::display
{

using vtbackend::Height;
using vtbackend::ImageSize;
using vtbackend::Width;

using vtbackend::ColumnCount;
using vtbackend::LineCount;
using vtbackend::PageSize;
using vtbackend::RGBAColor;

using text::DPI;

using namespace std::string_literals;
using namespace std::string_view_literals;
using namespace std;

using std::chrono::steady_clock;

using actions::Action;

// {{{ helper
namespace
{

    std::string unhandledExceptionMessage(std::string_view const& where, exception const& e)
    {
        return std::format("{}: Unhandled exception caught ({}). {}", where, typeid(e).name(), e.what());
    }

    void reportUnhandledException(std::string_view const& where, exception const& e)
    {
        displayLog()("{}", unhandledExceptionMessage(where, e));
        cerr << unhandledExceptionMessage(where, e) << '\n';
    }

} // namespace
// }}}

// {{{ Display creation and QQuickItem overides
TerminalDisplay::TerminalDisplay(QQuickItem* parent):
    QQuickItem(parent),
    _startTime { steady_clock::time_point::min() },
    _updateTimer(this),
    _autoScrollTimer(this)
{
    // Seed the DPI dedup baseline in the BODY, not the member-init list: fontDPI() -> contentScale()
    // reads _forcedFontDpiProvider (and any future member), and init-list evaluation follows member
    // DECLARATION order — an init-list call would read members that are not initialized yet (UB,
    // trapped by UBSan as soon as a real display was constructed).
    _lastFontDPI = fontDPI();

    startupLog()("TerminalDisplay constructed (QML component instantiation reached)");
    auto const timer = ScopedTimer(startupLog, "TerminalDisplay constructor");
    initializeDisplayResources();

    setFlag(Flag::ItemIsFocusScope);
    setFlag(Flag::ItemHasContents);
    setAcceptedMouseButtons(Qt::MouseButton::AllButtons);
    setAcceptHoverEvents(true);

#if QT_CONFIG(im)
    setFlag(Flag::ItemAcceptsInputMethod);
    // updateInputMethod(Qt::ImEnabled | Qt::ImCursorRectangle | Qt::ImFont | Qt::ImAnchorRectangle |
    // Qt::ImHints);
    updateInputMethod(Qt::ImQueryAll);
#endif

    connect(this, &QQuickItem::windowChanged, this, &TerminalDisplay::handleWindowChanged);

    _updateTimer.setSingleShot(true);
    connect(&_updateTimer, &QTimer::timeout, this, &TerminalDisplay::scheduleRedraw, Qt::QueuedConnection);

    _autoScrollTimer.setInterval(50);
    connect(
        &_autoScrollTimer, &QTimer::timeout, this, &TerminalDisplay::onAutoScrollTick, Qt::QueuedConnection);
}

TerminalDisplay::~TerminalDisplay()
{
    displayLog()("Destroying terminal widget.");
    // ~QQuickItem (running AFTER this body) unparents the item and re-emits windowChanged(nullptr)
    // — at which point the TerminalDisplay part of `this` no longer exists, so the ctor's
    // windowChanged→handleWindowChanged connection would invoke a slot on a half-destroyed object
    // (Qt asserts in debug, UB in release). Sever it first: a dying display has no window business.
    QObject::disconnect(this, &QQuickItem::windowChanged, this, &TerminalDisplay::handleWindowChanged);

    // The render node's prepare()/render() run in the scene graph's RENDER phase — the GUI thread is
    // NOT blocked there, so a frame may be mid-flight inside this display right now. Two steps make
    // destruction race-free: (1) publish null in the shared liveness cell, so any FUTURE node callback
    // no-ops; (2) fence out the possibly in-flight frame that loaded `this` before the store — a
    // NoStage render job runs on the render thread as soon as the current frame completes (or runs
    // synchronously right here when no render loop is active), so once it releases, the render thread
    // can no longer be executing display/renderer code.
    _nodeLiveness->store(nullptr, std::memory_order_release);
    if (auto* win = window(); win != nullptr)
    {
        QSemaphore fence;
        win->scheduleRenderJob(QRunnable::create([&fence]() { fence.release(); }), QQuickWindow::NoStage);
        fence.acquire();
    }

    // Free the RHI renderer. It is normally released by the scene-graph node's releaseResources() or
    // by sceneGraphInvalidated() → cleanup(), but neither fires when the item is still parented into a
    // window that is simply destroyed (the offscreen/bare-QQuickWindow teardown path) — the renderer
    // and its GPU pipelines would leak. The render-thread fence above has quiesced the render thread,
    // so releasing the RHI resources here is safe; destroyRenderer() is idempotent, so a later
    // node/invalidation teardown that also runs it is a no-op.
    destroyRenderer();

    // Evict this display from the manager's per-display bookkeeping before it is freed, so no
    // dangling TerminalDisplay* key (or its dangling currentSession) survives in _displayStates. Use
    // the cached manager rather than _session->getTerminalManager(): a closed split pane is destroyed
    // after its session was already detached (_session == nullptr), so the session route would miss it.
    if (_manager != nullptr)
        _manager->detachDisplay(this);
    if (_session)
        _session->detachDisplay(*this);
}

void TerminalDisplay::setSession(TerminalSession* newSession)
{
    displayLog()("TerminalDisplay::setSession: {} -> {}\n", (void*) _session, (void*) newSession);
    if (_session == newSession)
        return;

    // A null newSession means "detach": the split-pane QML binding (PaneNode.qml
    // `session: root.node ? root.node.session : null`) transiently resolves to null while a pane's Loader
    // is still active during a split collapse, so the `session` property WRITE can arrive here as nullptr.
    // The rest of this function unconditionally dereferences newSession (profile(), start(), attachDisplay,
    // ...), so route a null through the existing detach path instead of segfaulting the whole window.
    if (newSession == nullptr)
    {
        releaseSession();
        return;
    }

    // This will print the same pointer address for `this` but a new one for newSession (model data).
    displayLog()("Assigning session to display({} <- {}): shell={}, terminalSize={}, fontSize={}, "
                 "contentScale={}",
                 (void const*) this,
                 (void const*) newSession,
                 newSession->profile().ssh.value().hostname.empty()
                     ? std::format("program={}", newSession->profile().shell.value().program)
                     : std::format("{}@{}:{}",
                                   newSession->profile().ssh.value().username,
                                   newSession->profile().ssh.value().hostname,
                                   newSession->profile().ssh.value().port),
                 newSession->profile().terminalSize.value(),
                 newSession->profile().fonts.value().size,
                 contentScale());

    if (_session)
    {
        QObject::disconnect(_session, &TerminalSession::titleChanged, this, &TerminalDisplay::titleChanged);
        _session->detachDisplay(*this);
    }

    _session = newSession;

    // Cache the manager so ~TerminalDisplay can self-evict from _displayStates even if this pane is
    // closed before it ever receives focus (focus-in is the other place the cache is set).
    if (auto* manager = newSession->getTerminalManager(); manager != nullptr)
        _manager = manager;

    // Inject the app-wide forced-DPI provider (one cached instance; see ContentScale.h) and refresh the
    // font DPI whenever the platform override changes at runtime.
    if (auto* provider = newSession->forcedFontDpiProvider();
        provider != nullptr && provider != _forcedFontDpiProvider)
    {
        _forcedFontDpiProvider = provider;
        connect(provider, &ForcedFontDpiProvider::changed, this, &TerminalDisplay::applyFontDPI);
    }

    QObject::connect(newSession, &TerminalSession::titleChanged, this, &TerminalDisplay::titleChanged);

    auto const imeEnabled = profile().inputMethodEditor.value();
    setFlag(Flag::ItemAcceptsInputMethod, imeEnabled);
    displayLog()("IME enabled: {}", imeEnabled);

    // NB: The window frame is owned by QML now. main.qml makes the window frameless on the platforms
    // that use the custom client-side TitleBar (the tab strip + window controls). The profile's
    // show_title_bar setting controls the *custom* bar's visibility (not the native frame, which
    // would otherwise double-decorate); main.qml binds the TitleBar's visibility to the CONTROLLER's
    // titleBarVisible. Only SEED the window default here (first-write-wins): re-applying the profile
    // value on every session rebind silently reverted a runtime ToggleTitleBar on each tab switch.
    // Announcements go through THIS display, which is the object an assistive client already knows
    // about (TerminalAccessible::installFactory hands out its interface). Installed on every session
    // bind, so a rebound session speaks through the display it is actually shown in.
    _session->setAnnouncer(std::make_unique<QtAnnouncer>(this));

    if (auto* controller = windowController())
    {
        controller->seedTitleBarVisible(profile().showTitleBar.value());
        // Tab-strip placement + visibility are window state seeded once from the configuration, same
        // first-write-wins contract as the title bar (so a runtime state is never reset on rebind).
        // They are global rather than per-profile: one window shows one tab bar, whichever profiles
        // its tabs happen to run.
        controller->seedTabBarPosition(_session->config().tabBarPosition.value());
        controller->seedTabBarVisibility(_session->config().tabBarVisibility.value());
    }

    if (!_renderer)
    {
        auto const timer = ScopedTimer(startupLog, "Renderer construction");
        _renderer = make_unique<vtrasterizer::Renderer>(
            _session->profile().terminalSize.value(),
            sanitizeFontDescription(profile().fonts.value(), fontDPI()),
            _session->terminal().colorPalette(),
            _session->config().renderer.value().textureAtlasHashtableSlots,
            _session->config().renderer.value().textureAtlasTileCount,
            _session->config().renderer.value().textureAtlasDirectMapping,
            _session->profile().hyperlinkDecoration.value().normal,
            _session->profile().hyperlinkDecoration.value().hover,
            _session->config().textScalingMethod.value());

        // setup once with the renderer creation
        applyFontDPI();
        notifyCellGeometryChanged();
    }

    _session->attachDisplay(*this); // NB: Requires Renderer to be instanciated to retrieve grid metrics.

    // Render ReGIS text through the real font engine: (re)build a text_shaper-backed rasterizer from
    // the now-bound session's profile font and inject it. Rebuilding when the font or DPI differs keeps
    // a tab switch or config reload from rendering ReGIS text in a stale typeface.
    updateReGISTextRasterizer();

    // Only now fork the child, and deliberately so: attachDisplay() above is what first tells the PTY
    // its pixel size, and that needs the Renderer's grid metrics, which need the font. Starting before
    // it meant openpty() baked ws_xpixel = 0 into the winsize the child was born with, and the real
    // size only arrived afterwards as a SIGWINCH. An application that reads TIOCGWINSZ once at startup
    // -- img2sixel, chafa, termbench-pro's image-bench -- never sees that correction and falls back to
    // guessing a cell size. UnixPty::resizeScreen() stashes a size reported before start() precisely so
    // this ordering works.
    {
        auto const timer = ScopedTimer(startupLog, "Session start");
        _session->start();
    }

    // Reconcile a session re-bound onto a display that is ALREADY rendering (a new tab, or switching to a
    // background tab) to the INCOMING session's own state — the single TerminalDisplay/_renderer is reused
    // across tabs, so anything left on it by the previous tab leaks unless re-seeded here. Only a
    // newly-spawned window's first session enforces the profile via createRenderer()'s one-time seed (while
    // _renderTarget is still null), so gating on hasRenderTarget() cleanly excludes that path.
    if (hasRenderTarget())
    {
        // Font is pane-local: the renderer keeps whatever font the last-active tab pushed, so without this
        // a font-size/-family change on one tab visibly persists after switching to another. setFontSize()
        // persists the runtime size into the session's own _profile.fonts, so re-seeding from
        // profile().fonts restores THIS session's value (never a stale profile default) — the property that
        // makes fonts safe to re-apply on rebind where a blanket profile re-apply is not. Done BEFORE the
        // grid refit so applyDisplaySizeToGrid() fits against the final cell size; setFonts() itself already
        // reflows the grid, and both steps are idempotent when nothing changed.
        setFonts(_session->profile().fonts.value());

        // attachDisplay() above only round-trips the session's stale page size
        // (resizeScreen(totalPageSize(), pixelSize())); for a live window the WINDOW is the size authority,
        // so refit the incoming session to this display's real current extent (grid AND child PTY).
        // Idempotent: applyResize() no-ops when the page already matches.
        applyDisplaySizeToGrid();
    }

    _session->terminal().setImageDecoder(
        [](vtbackend::ImageFormat format,
           std::span<uint8_t const> data,
           vtbackend::ImageSize& size) -> std::optional<vtbackend::Image::Data> {
            if (format != vtbackend::ImageFormat::PNG)
                return std::nullopt;

            QImage image;
            image.loadFromData(static_cast<uchar const*>(data.data()), static_cast<int>(data.size()));
            if (image.isNull())
                return std::nullopt;

            image = image.convertToFormat(QImage::Format_RGBA8888);

            size = vtbackend::ImageSize { vtbackend::Width::cast_from(image.width()),
                                          vtbackend::Height::cast_from(image.height()) };

            auto const rowBytes = static_cast<size_t>(image.width()) * 4;
            vtbackend::Image::Data pixels;
            pixels.resize(static_cast<size_t>(image.height()) * rowBytes);
            auto* p = pixels.data();
            for (auto const row: std::views::iota(0, image.height()))
            {
                memcpy(p, image.constScanLine(row), rowBytes);
                p += rowBytes;
            }
            return pixels;
        });

    emit sessionChanged(newSession);
}

void TerminalDisplay::releaseSession()
{
    if (!_session)
        return;
    displayLog()("TerminalDisplay::releaseSession: dropping session {} (taken over by another display)",
                 (void*) _session);
    QObject::disconnect(_session, &TerminalSession::titleChanged, this, &TerminalDisplay::titleChanged);
    // Clear the session's back-pointer as well: on the transient-null collapse path (setSession(nullptr))
    // no other display takes the session over before this one may be destroyed, and ~TerminalDisplay
    // skips detachDisplay() once _session is null — leaving the session posting into a freed display.
    // Guarded on the back-pointer because the hand-off path (TerminalSession::attachDisplay) repoints
    // it right after this call returns.
    if (_session->display() == this)
        _session->detachDisplay(*this);
    _session = nullptr;
    emit sessionChanged(nullptr);
}

void TerminalDisplay::applyDisplaySizeToGrid()
{
    if (!_session || !_renderTarget)
        return;

    if (width() == 0.0 || height() == 0.0)
        // This can happen when the window is minimized, or when the window is not yet fully initialized.
        return;

    // Window->grid, the ONLY reaction to a size change: floor this item's device-pixel extent to a grid
    // and apply it; the sub-cell remainder renders as background padding. This path never mutates the
    // QWindow — the WM's size is obeyed as-is (grid alignment during interactive resizes is the WM's job,
    // via the size-increment hint where the platform supports it) — so a resize event can never re-enter
    // itself. Grid->window resizes exist only for content-driven requests through the WindowController.
    auto const deviceSize = geometry::availableDevicePixels(width(), height(), contentScale());
    displayLog()("Display size changed to {}x{} virtual ({} device).", width(), height(), deviceSize);
    applyResize(deviceSize, *_session, *_renderer);
}

void TerminalDisplay::geometryChange(QRectF const& newGeometry, QRectF const& oldGeometry)
{
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    // One reflow per committed geometry change (position-only moves are irrelevant to the grid).
    if (newGeometry.size() != oldGeometry.size())
        applyDisplaySizeToGrid();
}

void TerminalDisplay::handleWindowChanged(QQuickWindow* newWindow)
{
    if (newWindow)
    {
        displayLog()("Attaching widget {} to window {}.", (void*) this, (void*) newWindow);
        connect(newWindow,
                &QQuickWindow::sceneGraphInitialized,
                this,
                &TerminalDisplay::onSceneGrapheInitialized,
                Qt::DirectConnection);

        connect(newWindow,
                &QQuickWindow::beforeSynchronizing,
                this,
                &TerminalDisplay::onBeforeSynchronize,
                Qt::DirectConnection);

        connect(newWindow,
                &QQuickWindow::sceneGraphInvalidated,
                this,
                &TerminalDisplay::cleanup,
                Qt::DirectConnection);

        // setSession() may have run before a window existed, in which case windowController() could not
        // route to THIS window's controller (it matches by the display's OS window) and the chrome
        // seeds were dropped or mis-targeted a no-op. Re-seed now that the window exists; all three are
        // first-write-wins per window, so an already-seeded window is unaffected. The tab bar pair is
        // seeded here too, not just the title bar: they are dropped by the very same race.
        if (_session != nullptr)
            if (auto* controller = windowController())
            {
                controller->seedTitleBarVisible(_session->profile().showTitleBar.value());
                controller->seedTabBarPosition(_session->config().tabBarPosition.value());
                controller->seedTabBarVisibility(_session->config().tabBarVisibility.value());
            }
    }
    else
        displayLog()("Detaching widget {} from window.", (void*) this);
}

/// Deletes an RhiRenderer on the render thread (where its RHI/GPU resources must be released).
/// Takes ownership of the renderer for its lifetime; the scheduling code hands over the unique_ptr.
class CleanupJob: public QRunnable
{
  public:
    explicit CleanupJob(std::unique_ptr<RhiRenderer> renderer): _renderer { std::move(renderer) } {}

    void run() override { _renderer.reset(); }

  private:
    std::unique_ptr<RhiRenderer> _renderer;
};

void TerminalDisplay::releaseResources()
{
    displayLog()("Releasing resources.");
    // QQuickItem::releaseResources() runs on the GUI thread, but GL teardown must happen on the render
    // thread with the context current — so defer it via a render job. The renderer may already be gone
    // (the scene-graph node's releaseResources() destroys it on the render thread), in which case there
    // is nothing to schedule.
    // window() can already be null if the item was unparented from the scene before releaseResources() runs;
    // scheduling a render job then would dereference null. The CleanupJob takes ownership of _renderTarget
    // and deletes it on the render thread, so only null the pointer once the job has taken it. Without a
    // window we must NOT null _renderTarget here — otherwise the deferred destroyRenderer() (via the
    // scene-graph node's releaseResources()/cleanup(), which runs on the render thread) would see nullptr and
    // the renderer would leak. Leaving it set lets that path delete it.
    if (_renderTarget && window())
    {
        // Hand ownership of the renderer to the render-thread job (which frees it where its GPU
        // resources live); std::move leaves _renderTarget null.
        window()->scheduleRenderJob(new CleanupJob(std::move(_renderTarget)),
                                    QQuickWindow::BeforeSynchronizingStage);
    }
}

void TerminalDisplay::cleanup()
{
    displayLog()("Cleaning up.");
    destroyRenderer();
}

void TerminalDisplay::onAutoScrollTick()
{
    if (_autoScrollState.direction != 0 && _session)
        _session->performAutoScroll(_autoScrollState.direction,
                                    vtbackend::LineCount(_autoScrollState.linesPerTick));
}

void TerminalDisplay::onRefreshRateChanged()
{
    // Guard like the sibling applyFontDPI slot: the screen hooks outlive both the session (transiently
    // null during a split collapse) and the window (a closed pane's item awaiting deferred deletion),
    // and refreshRate() dereferences window()->screen().
    if (_session == nullptr || window() == nullptr)
        return;
    auto const rate = refreshRate();
    displayLog()("Refresh rate changed to {}.", rate.value);
    _session->terminal().setRefreshRate(rate);
}

void TerminalDisplay::configureScreenHooks()
{
    Require(window());

    QScreen* screen = window()->screen();
    if (screen == _hookedScreen)
        return;

    // Re-wire the per-screen hooks to the CURRENT screen: they were previously bound once to the
    // creation-time screen, so after a monitor move refresh-rate/DPI signals kept arriving from the
    // wrong screen.
    if (_hookedScreen != nullptr)
        disconnect(_hookedScreen, nullptr, this, nullptr);
    _hookedScreen = screen;

    connect(window(), &QWindow::screenChanged, this, &TerminalDisplay::onScreenChanged, Qt::UniqueConnection);
    if (screen != nullptr)
    {
        connect(screen, SIGNAL(refreshRateChanged(qreal)), this, SLOT(onRefreshRateChanged()));
        connect(screen, SIGNAL(logicalDotsPerInchChanged(qreal)), this, SLOT(applyFontDPI()));
    }
}

void TerminalDisplay::applyContentScaleChange()
{
    applyFontDPI();
    // Without a render target, applyFontDPI() only STAGES the font reload (its no-target branch);
    // publishedCellSize() would keep the pre-change metrics and any geometry derived from it would be
    // stale. The staged apply needs no GPU, so materialize it now. With a render target this is a
    // no-op (applyFontDPI already applied synchronously).
    if (_renderer && _session && window())
        applyStagedFontReconfigNow();
}

void TerminalDisplay::onScreenChanged()
{
    displayLog()("Screen changed.");
    configureScreenHooks(); // re-home the per-screen hooks to the new screen
    applyFontDPI();
    // Re-derive the screen-dependent terminal facts from the ACTUAL screen (the window-level
    // grid-preserving resize, if any, is the controller's scale-settlement handler's job).
    if (_session != nullptr && window() != nullptr && window()->screen() != nullptr)
    {
        _session->terminal().setRefreshRate(refreshRate());
        _session->updateImageCanvasCeiling();
    }
}

void TerminalDisplay::applyFontDPI()
{
    auto const newFontDPI = fontDPI();
    if (newFontDPI == _lastFontDPI)
        return;

    displayLog()("Applying DPI {} (via content scale {}, {}).",
                 newFontDPI,
                 contentScale(),
                 window() ? "Window present" : "No window");

    // NB: _lastFontDPI is the dedup guard above; it is committed only once the renderer confirms it
    // actually applied the new DPI (see end of this function). Committing it here — before the staged
    // reload, which applyPendingReconfig() can fail and swallow (atlas reallocation, FreeType
    // instantiation) — would record success that never happened, and the guard above would then
    // permanently skip the retry when the same DPI is reported again, stranding the wrong DPI.

    // logDisplayInfo();

    if (!_session)
        return;

    Require(_renderer);

    // Stage a DPI-only change. setFontDPI() merges the new DPI into whatever font descriptions are
    // already staged (so a concurrent font-family change from a config reload is not clobbered), rather
    // than rebuilding the request from the live descriptions.
    _renderer->setFontDPI(newFontDPI);

    if (!_renderTarget)
    {
        // During createRenderer() the render target does not exist yet; the staged change is
        // materialized explicitly there via applyStagedReconfigDuringSetup(). With no render target
        // there is also no frame to drive. The WM size hints still depend on the DPR directly, so
        // refresh those (they do not need the render target) and return.
        //
        // _lastFontDPI is deliberately NOT committed here: the staged DPI is not materialized yet (the
        // renderer still publishes the old DPI), so there is nothing to confirm. createRenderer() commits
        // it right after it materializes the staged reconfig — committing it speculatively here would
        // record a success that has not happened.
        if (window())
        {
            notifyCellGeometryChanged();
        }
        return;
    }

    // Apply the staged DPI change synchronously and re-derive geometry against the new cell size now,
    // via the shared policy used for every discrete font reconfiguration (see applyStagedFontReconfigNow).
    auto const cellSizeChanged = applyStagedFontReconfigNow();

    // The WM size-increment hint derives from the DPR/contentScale, not only from the cell pixel size,
    // so a DPI change must refresh it even when the new cell metrics round to the SAME pixel size
    // (cellSizeChanged == false, so applyStagedFontReconfigNow() did not run the geometry recompute).
    // When the cell size did change, applyStagedFontReconfigNow() already ran this.
    if (!cellSizeChanged && window())
    {
        notifyCellGeometryChanged();
    }

    scheduleRedraw();

    // Commit the dedup guard only now that applyStagedReconfigDuringSetup() has run synchronously and the
    // renderer published the new DPI. If the staged reload threw (applyPendingReconfig() catches and keeps
    // the previous font), the published DPI stays old, _lastFontDPI is left unchanged, and the next
    // identical DPI signal correctly retries instead of being skipped forever. publishedFontDPI() reads
    // just the DPI under the lock (no full FontDescriptions copy).
    if (_renderer->publishedFontDPI() == newFontDPI)
        _lastFontDPI = newFontDPI;
}

void TerminalDisplay::logDisplayInfo()
{
    if (!_session)
        return;

    Require(_renderer);

    // clang-format off
    auto const fontSizeInPx = static_cast<int>(ceil((
        profile().fonts.value().size.pt / 72.0) * average(fontDPI())
    ));
    auto const normalScreenSize = vtbackend::ImageSize {
        Width::cast_from(window()->screen()->size().width()),
        Height::cast_from(window()->screen()->size().height())
    };
    auto const actualScreenSize = normalScreenSize * window()->effectiveDevicePixelRatio();
    // Snapshot the mutex-guarded grid metrics and font descriptions once each, then read fields off the
    // locals: gridMetrics()/fontDescriptions() each take _reconfigMutex and deep-copy a full struct, so
    // reading them per line (5 + 2 times) would do 7 locked deep copies for one diagnostic dump.
    auto const gm = gridMetrics();
    auto const fd = _renderer->fontDescriptions();
#if defined(CONTOUR_BUILD_TYPE)
    displayLog()("[FYI] Build type          : {}", CONTOUR_BUILD_TYPE);
#endif
    displayLog()("[FYI] Application PID     : {}", QCoreApplication::applicationPid());
    displayLog()("[FYI] Qt platform         : {}", QGuiApplication::platformName().toStdString());
    displayLog()("[FYI] Refresh rate        : {} Hz", refreshRate().value);
    displayLog()("[FYI] Screen size         : {}", actualScreenSize);
    displayLog()("[FYI] Device pixel ratio  : {}", window()->devicePixelRatio());
    displayLog()("[FYI] Effective DPR       : {}", window()->effectiveDevicePixelRatio());
    displayLog()("[FYI] Content scale       : {}", contentScale());
    displayLog()("[FYI] Font DPI            : {} ({})", fontDPI(), fd.dpi);
    displayLog()("[FYI] Font size           : {} ({} px)", fd.size, fontSizeInPx);
    displayLog()("[FYI] Cell size           : {} px", gm.cellSize);
    displayLog()("[FYI] Page size           : {}", gm.pageSize);
    displayLog()("[FYI] Font baseline       : {} px", gm.baseline);
    displayLog()("[FYI] Underline position  : {} px", gm.underline.position);
    displayLog()("[FYI] Underline thickness : {} px", gm.underline.thickness);
    // clang-format on
}

void TerminalDisplay::onSceneGrapheInitialized()
{
    displayLog()("onSceneGrapheInitialized ({}x{}, DPR {})", width(), height(), contentScale());
}

void TerminalDisplay::logRhiBackendInfoOnce()
{
    // Complements the "[FYI] Qt platform" line of the configuration dump: which rendering
    // implementation Qt RHI actually resolved (OpenGL/Vulkan/Metal/D3D11, and the device driving
    // it). QQuickWindow::rhi() is still null when sceneGraphInitialized fires, so this runs on the
    // first sync phase instead — on the render thread with the RHI (and, for the OpenGL backend,
    // its context) live, which also lets it report the real context version and profile.
    if (_rhiBackendLogged)
        return;
    auto const* rhi = window() != nullptr ? window()->rhi() : nullptr;
    if (rhi == nullptr)
        return;
    _rhiBackendLogged = true;

    displayLog()("[FYI] Qt RHI backend      : {} on {}",
                 rhi->backendName(),
                 rhi->driverInfo().deviceName.toStdString());
    if (auto const* glContext = QOpenGLContext::currentContext())
    {
        auto const profileName = [](QSurfaceFormat::OpenGLContextProfile profile) {
            switch (profile)
            {
                case QSurfaceFormat::CoreProfile: return "core";
                case QSurfaceFormat::CompatibilityProfile: return "compatibility";
                case QSurfaceFormat::NoProfile: break;
            }
            return "no profile";
        };
        auto const format = glContext->format();
        displayLog()("[FYI] OpenGL context      : {}.{} {} ({})",
                     format.majorVersion(),
                     format.minorVersion(),
                     profileName(format.profile()),
                     glContext->isOpenGLES() ? "OpenGL ES" : "desktop OpenGL");
    }
}

void TerminalDisplay::onBeforeSynchronize()
{
    // Reached on the render thread during the sync phase. Closing a split pane unparents this item from the
    // scene graph, so window() starts returning null while the TerminalDisplay object is still alive and
    // still connected to beforeSynchronizing (QQuickItem clears its window before ~QObject runs the
    // this-context auto-disconnect). _session is nulled independently by releaseSession(), so it is not a
    // reliable proxy for window() liveness — guard window() too, or line 795's window()->screen() faults on
    // a null QWindow. Matches the window() guards the sibling render paths already use (see recordFrameRhi,
    // scheduleRedraw).
    if (!_session || !window())
        return;

    logRhiBackendInfoOnce();

    if (width() < 1.0 || height() < 1.0)
        // e.g. this can happen when the window is not yet fully initialized
        return;

    if (_sessionChanged)
    {
        _sessionChanged = false;
        createRenderer();
    }

    if (!_renderTarget)
    {
        // This is the first call, so create the renderer (on demand) now.
        createRenderer();

        // Also check if the terminal terminated faster than the frontend needed to render the first frame.
        if (terminal().device().isClosed())
        {
            // Inform the session — but NOT synchronously: onBeforeSynchronize() runs on the render thread
            // (Qt::DirectConnection) during the sync phase, and onClosed() emits sessionClosed, whose
            // TerminalSessionManager::removeSession slot prunes the pane and tears this display down. Doing
            // that mid-sync frees state the remainder of the sync/render still dereferences → a teardown
            // SEGV on the render thread (seen intermittently on fast-exiting sessions). Post it to the
            // session's own thread instead, matching how every other GUI-mutating callback in this render
            // path is deferred (and how TerminalSession fires onClosed() from its exit watcher). The inner
            // guard covers the display being torn down between this post and its dispatch.
            postToObject(_session, [session = _session]() { session->onClosed(); });
        }
    }

    // No per-frame geometry reconciliation here: the grid follows the item's committed geometry via
    // geometryChange() on the GUI thread — every surface change the WM commits (including a tiling WM's
    // initial map) arrives there. The render size itself is published from updatePaintNode, and the
    // placement transform comes from the scene graph at render time.
}

void TerminalDisplay::createRenderer()
{
    Require(!_renderTarget);
    Require(_renderer);
    Require(_session);
    Require(window());

    // Catch DPR corrections that occurred between setSession() and first render
    // (e.g., Qt correcting from integer-ceiling DPR=2 to actual fractional DPR=1.5
    // on KDE/Wayland). This reloads fonts at the correct DPI before creating the
    // render target, ensuring correct cell metrics from the start.
    applyFontDPI();

    // applyFontDPI() only *stages* the font reload (its no-render-target branch ran above): the render
    // thread is not running yet and there is no frame to apply it, so materialize it now to read the
    // correct cell size for the texture atlas tile. applyStagedReconfigDuringSetup() also consumes the
    // "font reconfig applied" one-shot signal raised by this setup-time apply and returns it — we
    // intentionally discard it: the geometry is sized correctly below (applyResize() + the implicit-size /
    // constraints setup), so the first painted frame must NOT also post a redundant resize/recompute that
    // would re-derive the page size on frame 1 and cause a brief geometry recompute/flicker at open.
    (void) _renderer->applyStagedReconfigDuringSetup();

    // The staged DPI is now materialized, but applyFontDPI()'s no-render-target branch could not commit its
    // _lastFontDPI dedup guard (the renderer had not yet published the new DPI when it ran). Commit it now,
    // so a later screen/DPI signal reporting this same (already-applied) DPI is correctly deduped instead
    // of triggering a full redundant font reload + geometry recompute + spurious resizeScreen()/SIGWINCH.
    _lastFontDPI = fontDPI();

    // applyFontDPI()'s no-render-target branch already refreshed the WM hints — but against the
    // still-stale (pre-correction) cell size, because the staged DPI was only materialized just above.
    // Refresh them now that the corrected cell size is live.
    if (window())
    {
        notifyCellGeometryChanged();
    }

    auto const textureTileSize = gridMetrics().cellSize;
    // The render target size is this item's device-pixel extent (the bottom-left-origin reference for the
    // inner smooth-scroll scissor); it is re-published each frame from updatePaintNode. The placement
    // within the window comes from the scene graph's transform at render time, not from a window-sized
    // surface, so only the item size is needed here. The configured window margin is owned by the
    // geometry layer (contour::geometry / helper.cpp applyResize -> PageMargin -> GridMetrics::map()),
    // not by the render target.
    auto const precalculatedTargetSize = [this]() -> ImageSize {
        auto const uiSize = ImageSize { Width::cast_from(width()), Height::cast_from(height()) };
        return uiSize * contentScale();
    }();

    if (displayLog)
    {
        auto const dpr = contentScale();
        auto const viewSize =
            ImageSize { Width::cast_from(width() * dpr), Height::cast_from(height() * dpr) };
        auto const windowSize = window()->size() * dpr;
        displayLog()("Creating renderer: {}x+{}y+{}z ({} DPR, {} viewSize, {}x{} windowSize)\n",
                     x(),
                     y(),
                     z(),
                     dpr,
                     viewSize,
                     windowSize.width(),
                     windowSize.height());
    }

    _renderTarget = std::make_unique<RhiRenderer>(precalculatedTargetSize, textureTileSize);
    _renderer->setRenderTarget(*_renderTarget);

    // The terminal no longer paints from the window's beforeRendering/afterRendering signals (which fired
    // after the whole QML scene was composited and let the terminal overpaint popups). Rendering is now
    // driven by the scene graph through TerminalRenderNode (see updatePaintNode/renderFrame), so it
    // composites in z-order. The renderer's one-time GL initialization happens lazily on the first
    // renderFrame(), where the scene graph's GL context is current.

    configureScreenHooks();

    // Seed the grid from this item's current extent (window->grid, floor semantics). During the first
    // sync the anchor layout may not have fully propagated yet; that is fine: geometryChange() re-derives
    // the grid the moment the committed geometry changes, and applyResize() no-ops once they match.
    applyResize(geometry::availableDevicePixels(width(), height(), contentScale()), *_session, *_renderer);

    // Defer configureDisplay() until the GUI thread processes QML binding propagation
    // and the window is committed at its final initial size (e.g. 1136x600 at DPR 1.5).
    // configureDisplay() drives setFonts()/resizeTerminalToDisplaySize() against the display's
    // metrics; running it synchronously here (render thread, GUI blocked) would compute geometry
    // from the stale pre-DPR-correction window size (e.g. 1115x585 at DPR 2.0) instead of the
    // committed final size, reverting the terminal to the wrong dimensions.
    // Re-check _session inside the lambda: post() runs it later on the GUI event loop, and the display can be
    // torn down (releaseSession -> _session == null) between this schedule and its dispatch, matching the
    // inner-guard pattern of the other post() lambdas here (863/1134).
    post([this]() {
        if (_session)
            _session->configureDisplay();
    });

    displayLog()("Implicit size: {}x{}", implicitWidth(), implicitHeight());
}

QMatrix4x4 TerminalDisplay::createModelMatrix() const
{
    QMatrix4x4 result;

    // Compose model matrix from our transform properties in the QML
    QQmlListProperty<QQuickTransform> transformations = const_cast<TerminalDisplay*>(this)->transform();
    auto const count = transformations.count(&transformations);
    for (int i = 0; i < count; i++)
    {
        QQuickTransform* transform = transformations.at(&transformations, i);
        transform->applyTo(&result);
    }

    return result;
}

void TerminalDisplay::prepareFrameRhi(QRhi* rhi,
                                      QRhiCommandBuffer* cb,
                                      QRhiRenderTarget* rt,
                                      QRhiRenderPassDescriptor* rpDesc,
                                      QMatrix4x4 const& itemToClip,
                                      QPoint itemOriginDevice)
{
    // _session is nulled by releaseSession() independently of _renderTarget teardown, so on pane close there
    // is a window where _renderTarget is still set but _session is already null. paint() (below) dereferences
    // the session via terminal()/_session->terminal(), which is assert-only (a no-op under NDEBUG) — so guard
    // _session here to avoid a null deref in release builds.
    if (!_renderTarget || !_session || rhi == nullptr || cb == nullptr || rt == nullptr || rpDesc == nullptr)
        return;

    // Snapshot the render target into a local for the rest of this frame. prepare() runs on the render
    // thread with the GUI thread UNBLOCKED, and releaseResources() nulls the _renderTarget member with a
    // std::move() on the GUI thread (handing ownership to a BeforeSynchronizingStage CleanupJob). Reading
    // the member on each line below is therefore a TOCTOU: it passes the guard above, then the GUI thread
    // moves-out the member mid-frame, and the next member access is a call on a null RhiRenderer (observed
    // as a SEGV in flushFrame() on fast-exiting sessions). The CleanupJob that actually deletes the object
    // runs at BeforeSynchronizingStage on THIS render thread, so it cannot overlap this render-phase call —
    // only the member null-out can, and the local pins a stable pointer for the whole frame.
    auto* const renderTarget = _renderTarget.get();

    // Lazily flag the renderer as initialized on the first prepare (sets host image-row alignment); the
    // actual RHI resources are built by createPipelines() below. This formerly lived in the renderFrame
    // initialize() call when the renderer still owned raw GL state.
    if (!renderTarget->initialized())
    {
        logDisplayInfo();
        renderTarget->initialize();
    }

    // Build (or reuse cached) graphics pipelines for the current render pass. prepare() runs before the
    // render pass is recording, which is the only valid point to create RHI graphics pipelines.
    renderTarget->createPipelines(rhi, rpDesc);
    if (!renderTarget->pipelinesReady())
        return;

    // Hand the per-frame RHI submission handles to the renderer so the staging done in paint() (terminal
    // render → execute()) queues its resource uploads onto this command buffer, before the scene graph
    // begins the render pass. The node clip (Qt's RenderState) is not available in prepare(); it is applied
    // in recordFrameRhi() at draw time. Any transient inner scissor captured during staging is stored raw
    // and intersected with the node clip there.
    renderTarget->beginFrame(rhi, cb, rt);

    // Install the scene graph's transform: the renderer feeds item-local, top-left-origin pixel vertices
    // through this combined projection * node-matrix, so the grid lands exactly where this item sits in
    // the window — no own ortho or translation needed. The render size and model matrix were snapshotted
    // in updatePaintNode while the GUI thread was blocked.
    renderTarget->setProjectionMatrix(itemToClip);

    // Install this item's device-pixel offset inside the render target: the rasterizer's transient inner
    // scissor is item-relative, but the GPU scissor addresses the window's framebuffer, so applyScissor()
    // must translate by this offset (a split pane is not at the window's bottom-left).
    renderTarget->setItemOriginInTarget(itemOriginDevice.x(), itemOriginDevice.y());

    // Run the terminal render (which calls the renderer's execute() one or more times to accumulate this
    // frame's geometry), then flush the accumulated vertex/atlas uploads onto the command buffer before the
    // scene graph begins the render pass. Use the pinned local for flushFrame() for the same reason as
    // above (paint() re-guards _renderTarget/_session internally and no-ops if either was torn down).
    paint();
    renderTarget->flushFrame();

    // Deliver a screenshot captured on the previous frame (its readback has completed by now), then, if a
    // capture is pending, replay this frame's staged draws into the offscreen screenshot texture and schedule
    // its readback. Both happen here in prepare(), before the scene graph begins its render pass — the only
    // point at which a fresh beginPass/endPass into the offscreen target is legal (passes cannot nest).
    // Uses the pinned local for the same GUI-thread-null-out reason as above.
    renderTarget->deliverScreenshot();
    if (renderTarget->screenshotRequested())
        renderTarget->recordScreenshotPass(rhi, cb);
}

void TerminalDisplay::recordFrameRhi(QSGRenderNode::RenderState const* state)
{
    if (!_renderTarget)
        return;

    // Pin the render target for this frame: like prepareFrameRhi(), this runs on the render thread with the
    // GUI thread unblocked, so releaseResources() can std::move the _renderTarget member to null between the
    // guard above and the member accesses below (a TOCTOU null call). The object itself is only deleted by a
    // BeforeSynchronizingStage job on this same render thread, so it cannot vanish mid-call — only the member
    // null-out races us, and the local pins a stable pointer.
    auto* const renderTarget = _renderTarget.get();

    // Install Qt's node clip for this frame (only available now, from the RenderState). recordDraws()
    // intersects it with each pass's transient inner scissor (smooth scroll / cursor) captured during
    // staging. std::nullopt when Qt is not clipping the node.
    if (state->scissorEnabled())
    {
        auto const r = state->scissorRect();
        renderTarget->setNodeScissorRect(
            ScissorRect { .x = r.x(), .y = r.y(), .width = r.width(), .height = r.height() });
    }
    else
        renderTarget->setNodeScissorRect(std::nullopt);

    // Issue the draw commands staged during prepareFrameRhi(), now that the scene graph's render pass is
    // recording. No-op if nothing was staged (e.g. pipelines were not ready in prepare()).
    renderTarget->recordDraws();

    // Forget the node clip (member only), so a later code path never intersects against a stale rectangle.
    renderTarget->clearNodeScissorRect();
}

void TerminalDisplay::paint()
{
    // We consider *this* the true initial start-time.
    // That shouldn't be significantly different from the object construction
    // time, but just to be sure, we'll update it here.
    if (_startTime == steady_clock::time_point::min())
        _startTime = steady_clock::now();

    // Everything below dereferences the session via terminal() (assert-only, so a null deref in release);
    // _session can be null here while _renderTarget still lives during pane-close teardown.
    if (!_renderTarget || !_session)
        return;

    try
    {
        [[maybe_unused]] auto const lastState = _state.fetchAndClear();

#if defined(CONTOUR_PERF_STATS)
        {
            ++renderCount_;
            auto const updateCount = stats_.updatesSinceRendering.exchange(0);
            auto const renderCount = stats_.consecutiveRenderCount.exchange(0);
            if (displayLog)
                displayLog()("paintGL/{}: {} renders, {} updates since last paint ({}/{}).",
                             renderCount_.load(),
                             renderCount,
                             updateCount,
                             lastState,
                             to_string(_session->terminal().renderBufferState()));
        }
#endif

        terminal().tick(steady_clock::now());

        auto const fontReconfigApplied = _renderer->render(terminal(), _renderingPressure);

        // The lazily-applied font/DPI change made the cell size current only now; re-derive page size
        // and the WM size hints against it (the triggering resize didn't).
        // render() consumes the "font reconfig applied" signal under _applyMutex and returns it, so this
        // render-thread path and the GUI-thread applyStagedReconfigNow() can never both consume it.
        //
        // The recompute must run on the GUI thread (it mutates Qt window state and resizes the terminal),
        // so it is post()ed and applied on frame N+1: for this one frame the new cell size is rendered
        // against the previous page size/margins. That single-frame divergence is an accepted trade-off
        // of keeping all grid-metrics mutation on the render thread (the deferral that fixed the
        // resize-time crashes); applying it inline here would touch Qt/terminal state off the GUI thread.
        if (fontReconfigApplied)
            post([this]() {
                // The display may be torn down between this post() (render thread) and its execution
                // (GUI thread): the session/renderer can be cleared and, crucially, the window may be
                // gone. recomputeGeometryAfterFontReconfig() requires a live window (Require(window())
                // inside it would std::abort()), so bail out unless the display is still fully alive.
                if (!_session || !_renderer || !window())
                    return;
                recomputeGeometryAfterFontReconfig();
            });

        if (_doDumpState)
        {
            doDumpStateInternal();
            _doDumpState = false;
        }

        if (_saveScreenshot)
        {
            // Deferred capture (RHI offscreen readback lands one frame later). Bind the destination now and
            // deliver when the pixels are ready. Clearing _saveScreenshot immediately prevents re-arming the
            // request every frame while the readback is in flight.
            auto const destination = _saveScreenshot.value();
            _saveScreenshot = std::nullopt;
            requestScreenshot([destination](QImage const& image) {
                std::visit(crispy::overloaded { [&](std::filesystem::path const& path) {
                                                   image.save(QString::fromStdString(path.string()));
                                               },
                                                [&](std::monostate) {
                                                    if (QClipboard* clipboard = QGuiApplication::clipboard();
                                                        clipboard != nullptr)
                                                        clipboard->setImage(image, QClipboard::Clipboard);
                                                } },
                           destination);
            });
        }

        // Schedule the next frame if needed. update() (re-evaluating the scene-graph node) must run on the
        // GUI thread, so it is post()ed from this render-thread path — mirroring scheduleRedraw().
        auto const requestUpdate = [this]() {
            post([this]() { update(); });
        };

        if (!_state.finish())
            requestUpdate();

        // Update the terminal's world clock, so nextRender() knows when to render next (if needed).
        terminal().tick(steady_clock::now());

        if (auto const timeoutOpt = terminal().nextRender(); timeoutOpt.has_value())
        {
            if (*timeoutOpt == chrono::milliseconds::min())
                requestUpdate();
            else
                post([this, timeout = *timeoutOpt]() { _updateTimer.start(timeout); });
        }
    }
    catch (exception const& e)
    {
        reportUnhandledException(__PRETTY_FUNCTION__, e);
    }
}

float TerminalDisplay::uptime() const noexcept
{
    using namespace std::chrono;
    auto const now = steady_clock::now();
    auto const uptimeMsecs = duration_cast<milliseconds>(now - _startTime).count();
    auto const uptimeSecs = static_cast<float>(uptimeMsecs) / 1000.0f;
    return uptimeSecs;
}

QSGNode* TerminalDisplay::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* /*data*/)
{
    // Runs on the render thread with the GUI thread blocked (safe to touch both). Create the RHI
    // renderer on demand (formerly done from the window's render signals) and the scene-graph node that
    // draws the terminal in z-order.
    if (width() < 1.0 || height() < 1.0 || !_session)
    {
        // Nothing to draw yet; drop any existing node so we recreate it once we have a size/session.
        delete oldNode;
        return nullptr;
    }

    if (!_renderTarget)
        createRenderer();

    // updatePaintNode runs during the synchronize phase with the GUI thread blocked, so this is the safe
    // point to read item geometry — the GUI thread cannot mutate it concurrently. Snapshot the item's
    // device-pixel render size here and push it to the renderer, rather than reading it from the render
    // thread in renderFrame() (where the GUI thread is unblocked). The render size is the bottom-left-origin
    // reference frame for the transient inner scissor.
    auto const geometry = itemDevicePixelGeometry();
    _renderTarget->setRenderSize(ImageSize { Width::cast_from(std::lround(geometry.width)),
                                             Height::cast_from(std::lround(geometry.height)) });

    // The model matrix (QML transforms applied to this item) is also read here while the GUI thread is
    // blocked; the scene-graph node only composes Qt's projection/node matrices on top of it at render.
    _renderTarget->setModelMatrix(createModelMatrix());

    auto* node = static_cast<TerminalRenderNode*>(oldNode);
    if (!node)
        node = new TerminalRenderNode(_nodeLiveness);
    else
        // Qt may have called releaseResources() (dropping the node's liveness link) on this same node
        // without destroying it — on scene-graph invalidation paths that reuse the node. Re-link it to
        // this display every frame, or a released-but-reused node would render nothing (black terminal).
        node->setLiveness(_nodeLiveness);

    // Content changes every frame the terminal is dirty; mark the node so Qt invokes render() again.
    node->markDirty(QSGNode::DirtyMaterial);
    return node;
}

void TerminalDisplay::releaseRenderResources()
{
    // Called by TerminalRenderNode on the render thread as the node is torn down — the right place to free
    // the RHI renderer's GPU resources, which must be released on the render thread.
    destroyRenderer();
}

void TerminalDisplay::destroyRenderer()
{
    // Single, idempotent renderer teardown shared by all entry points (the scene-graph node's
    // releaseResources, sceneGraphInvalidated → cleanup). delete of nullptr is a no-op, so calling this
    // more than once across those paths frees the renderer exactly once. Must run on the render thread
    // (the RHI resources it owns are released there).
    //
    // The long-lived vtrasterizer::Renderer survives this target: sever its references FIRST, or the
    // next createRenderer() setup pass (staged reconfig before the new target is installed) reads the
    // freed target on the render thread. The scene graph destroys/re-creates its context routinely —
    // grabbing an unexposed window, GPU loss, invalidation — so this cycle is a normal code path.
    if (_renderer)
        _renderer->detachRenderTarget();
    _renderTarget.reset();
}
// }}}

// {{{ Qt Display Input Event handling & forwarding
//
// Every handler below forwards to a send*Event(..., *_session) that binds the session by reference. A
// session-less display must ignore all input: during a split/collapse a pane can be shown while its
// _session is transiently null (the QML `session` binding rebinds via setSession/releaseSession across the
// hand-off), and Qt still delivers hover/mouse/key events to it. Dereferencing *_session then binds a null
// reference (UB -> crash on the first _session access, e.g. terminal().totalPageSize() during a hover). A
// display with no session has nothing to route input to, so early-out.
void TerminalDisplay::keyPressEvent(QKeyEvent* keyEvent)
{
    if (!_session)
        return;
    sendKeyEvent(keyEvent,
                 keyEvent->isAutoRepeat() ? vtbackend::KeyboardEventType::Repeat
                                          : vtbackend::KeyboardEventType::Press,
                 *_session);
}

void TerminalDisplay::keyReleaseEvent(QKeyEvent* keyEvent)
{
    if (!_session || keyEvent->isAutoRepeat())
        return;
    sendKeyEvent(keyEvent, vtbackend::KeyboardEventType::Release, *_session);
}

void TerminalDisplay::wheelEvent(QWheelEvent* event)
{
    if (!_session)
        return;
    sendWheelEvent(event, *_session);
}

void TerminalDisplay::mousePressEvent(QMouseEvent* event)
{
    if (!_session)
        return;
    sendMousePressEvent(event, *_session);
}

void TerminalDisplay::mouseMoveEvent(QMouseEvent* event)
{
    if (!_session)
        return;
    sendMouseMoveEvent(event, *_session);

    // Start, update, or stop auto-scroll based on whether the mouse is outside the content area
    // while the left button is pressed (i.e., during a drag-selection).
    if (event->buttons() & Qt::LeftButton)
    {
        _autoScrollState = computeAutoScrollInfo(event, *_session);
        if (_autoScrollState.direction != 0)
        {
            if (!_autoScrollTimer.isActive())
                _autoScrollTimer.start();
        }
        else
        {
            _autoScrollTimer.stop();
        }
    }
}

void TerminalDisplay::hoverMoveEvent(QHoverEvent* event)
{
    QQuickItem::hoverMoveEvent(event);
    if (!_session)
        return;
    sendMouseMoveEvent(event, *_session);
}

void TerminalDisplay::hoverLeaveEvent(QHoverEvent* event)
{
    QQuickItem::hoverLeaveEvent(event);
    if (!_session)
        return;

    // Nothing is under the pointer once it is outside the item, so the hyperlink tooltip goes with it.
    // The pointing-hand cursor is reset for the same reason: without a leave handler it survived the
    // pointer leaving a link at the item's edge, which was a small pre-existing wart.
    _session->onPointerLeft();
}

void TerminalDisplay::mouseReleaseEvent(QMouseEvent* event)
{
    if (!_session)
        return;
    _autoScrollTimer.stop();
    _autoScrollState = {};
    sendMouseReleaseEvent(event, *_session);
}

void TerminalDisplay::focusInEvent(QFocusEvent* event)
{
    QQuickItem::focusInEvent(event);

    if (_session)
    {
        // Cache the manager so ~TerminalDisplay can self-evict from _displayStates even after the
        // session is gone (see the destructor). This focus-in is also where the display first enters
        // _displayStates (FocusOnDisplay), so the cache and the registration are set together.
        _manager = _session->getTerminalManager();
        _manager->FocusOnDisplay(this); // record the active display before moving VT focus
        // Route VT focus through the manager's single authority (model tab/pane changes use the same
        // seam), so it stays correct even when no Qt focus event fires. Dimming is done in QML.
        _manager->setFocusedSession(_session);
    }
}

void TerminalDisplay::focusOutEvent(QFocusEvent* event)
{
    QQuickItem::focusOutEvent(event);
    // A focus loss clears VT focus, but only if this session still holds it: when focus moved to
    // another pane/tab the model already re-pointed it, and this stale focus-out must not cancel that.
    if (_session)
        if (auto* manager = _session->getTerminalManager())
            manager->clearFocusIfCurrent(_session);

    // Whatever this pane last told an assistive client is now stale — the client is following some other
    // pane. Forgetting it is what lets this pane re-announce itself on regaining focus, even if its caret
    // never moved in the meantime.
    resetAccessibleCaret();
}

#if QT_CONFIG(im)
void TerminalDisplay::inputMethodEvent(QInputMethodEvent* event)
{
    if (!_session)
        return;
    terminal().updateInputMethodPreeditString(event->preeditString().toStdString());

    if (!event->commitString().isEmpty())
    {
        assert(event->preeditString().isEmpty());
        QKeyEvent keyEvent(QEvent::KeyPress, 0, Qt::NoModifier, event->commitString());
        keyPressEvent(&keyEvent);
    }

    event->accept();
}
#endif

QVariant TerminalDisplay::inputMethodQuery(Qt::InputMethodQuery query) const
{
    // Qt may query while the pane is still wiring up (or tearing down); without a renderer or
    // session there is no grid to answer from.
    if (!_renderer || !_session)
        return QQuickItem::inputMethodQuery(query);

    // One locked GridMetrics copy per query (this fires on every keystroke with an IME active): it
    // yields the cell size AND the page margin tear-free, where two separate reads could straddle a
    // concurrent font apply.
    auto const metrics = _renderer->gridMetrics();

    switch (query)
    {
        case Qt::ImCursorRectangle:
            // Item-local logical coordinates (QQuickWindow maps them to window/global space, so a
            // pane inside a split needs no extra offset); the grid is inset by the page margin, and
            // a double-width character under the cursor widens the rect to the full glyph.
            if (terminal().isCursorInViewport())
            {
                auto const cursor = terminal().currentScreen().cursor().position;
                auto const cellWidth = terminal().currentScreen().cellWidthAt(cursor);
                return imeCursorRectangle(
                    metrics.pageMargin, metrics.cellSize, cursor, cellWidth, contentScale());
            }
            return QRectF();
        case Qt::ImCursorPosition:
            // Qt contract: the cursor's CHARACTER index within ImSurroundingText (the current
            // line), i.e. the grid column — not a pixel offset.
            if (terminal().isCursorInViewport())
                return unbox<int>(terminal().currentScreen().cursor().position.column);
            return 0;
        case Qt::ImSurroundingText:
            // return the text from the current line
            if (terminal().isCursorInViewport())
                return QString::fromStdString(
                    terminal().currentScreen().lineTextAt(terminal().currentScreen().cursor().position.line));

            return QString();
        case Qt::ImCurrentSelection:
            // Nothing selected.
            return QString();
        default:
            // bubble up
            break;
    }
    return QQuickItem::inputMethodQuery(query);
}

bool TerminalDisplay::event(QEvent* event)
{
    try
    {
        if (event->type() == QEvent::Close)
        {
            assert(_session);
            _session->pty().close();
            emit terminated();
        }

        return QQuickItem::event(event);
    }
    catch (std::exception const& e)
    {
        std::cout << std::format("Unhandled exception for event {}: {}\n",
                                 (unsigned) event->type(),
                                 QMetaEnum::fromType<QEvent::Type>().valueToKey(event->type()));
        reportUnhandledException(__PRETTY_FUNCTION__, e);
        return false;
    }
}
// }}}

// {{{ helpers
void TerminalDisplay::onScrollBarValueChanged(int value)
{
    terminal().viewport().scrollTo(vtbackend::ScrollOffset::cast_from(value));
    scheduleRedraw();
}

double TerminalDisplay::contentScale() const
{
    // THE content-scale read: the cached app-wide forced-DPI override (injected in setSession) wins over
    // the window's DPR; pre-window the resolver falls back to 1.0. One shared implementation with the
    // pre-show sizing path (see display/ContentScale.h) — no per-call file I/O.
    return contentScaleForWindow(window(), _forcedFontDpiProvider);
}

TerminalDisplay::DevicePixelGeometry TerminalDisplay::itemDevicePixelGeometry() const
{
    // The item's device-pixel extent. Scene position is no longer needed: the scene graph supplies the
    // item→clip transform at render time (see renderFrame), so callers want only the size.
    auto const dpr = contentScale();
    return DevicePixelGeometry {
        .width = width() * dpr,
        .height = height() * dpr,
    };
}

WindowController* TerminalDisplay::windowController()
{
    return _manager != nullptr ? _manager->controllerForDisplay(this) : nullptr;
}

void TerminalDisplay::notifyCellGeometryChanged()
{
    // Window-level hints are the controller's job (the window-geometry authority); it derives them from
    // this display's cell size, margins, content scale and the QML-declared chrome. Displays without a
    // controller (offscreen tests) simply have no WM hints to maintain.
    if (auto* controller = windowController())
        controller->updateSizeHintsFor(*this);
}
// }}}

// {{{ TerminalDisplay: attributes
vtbackend::RefreshRate TerminalDisplay::refreshRate() const
{
    auto* const screen = window()->screen();
    if (!screen)
        return { vtbackend::RefreshRate { 30.0 } };

    auto const systemRefreshRate = vtbackend::RefreshRate { static_cast<double>(screen->refreshRate()) };
    return systemRefreshRate;
}

DPI TerminalDisplay::fontDPI() const noexcept
{
    return DPI { .x = 96, .y = 96 } * contentScale();
}

bool TerminalDisplay::isFullScreen() const
{
    return window()->visibility() == QQuickWindow::Visibility::FullScreen;
}

vtbackend::ImageSize TerminalDisplay::pixelSize() const
{
    assert(_session);
    return geometry::requiredPixelsForPage(
        _session->terminal().totalPageSize(),
        _renderer->publishedCellSize(),
        geometry::scaled(toGeometryMargins(_session->profile().margins.value()), contentScale()));
}

vtbackend::ImageSize TerminalDisplay::reportedPixelSize(vtbackend::PageSize totalPageSize) const
{
    assert(_session);
    // Report the unit the cell is an integer in -- for this renderer, device pixels.
    //
    // An application divides a reported extent by the grid to recover the cell, so the report is only
    // usable if that division is exact. Our cell is the font's advance in device pixels (an int), and
    // at a fractional scale it has no exact logical counterpart: 17 / 1.75 = 9.714. Dividing the scale
    // out floors each axis on its own, which does not merely shrink the report -- it changes the cell's
    // ASPECT RATIO (17x39 -> 9x22 loses 7.4% of the width but 1.3% of the height). A full-page image
    // sized from that cell is then aspect-fitted (ImageResize::ResizeToFit) into the device grid, and
    // its std::min() honors the less-damaged axis and letterboxes the other -- a ~6% gap down one side.
    //
    // Device reporting makes the round-trip exact, so the fit scale is 1.0 and the image lands 1:1 at
    // the display's own resolution. Konsole reports logical for the same reason in reverse: its cell is
    // QFontMetrics::horizontalAdvance(), an int in LOGICAL pixels (it ignores the scale when drawing
    // images, so they blur but never mis-size). The unit is not the principle; exactness is.
    //
    // Logical stays available for comparing against such a terminal at an equal canvas size. It is exact
    // only where the scale divides both cell axes evenly (e.g. 1.0, or 20x40 at 2.0), and letterboxes by
    // the floor error otherwise.
    return geometry::reportedPixelsForPage(
        totalPageSize, _renderer->publishedCellSize(), reportedPixelScale());
}

double TerminalDisplay::reportedPixelScale() const
{
    assert(_session);
    return _session->profile().pixelReporting.value() == config::PixelReporting::Device ? 1.0
                                                                                        : contentScale();
}

vtbackend::ImageSize TerminalDisplay::cellSize() const
{
    // Lock-free published cell-size read; avoids taking the renderer's reconfig mutex and copying the
    // whole GridMetrics struct just to extract the cell size on these hot UI paths.
    return _renderer->publishedCellSize();
}
// }}}

// {{{ TerminalDisplay: (user requested) actions
void TerminalDisplay::post(std::function<void()> fn)
{
    postToObject(this, std::move(fn));
}

namespace
{
    /// This display's accessibility interface, if one has been handed out and is still alive.
    ///
    /// Queried rather than held: the interface is created on demand by the factory and owned by Qt's
    /// accessibility cache, so a stored pointer could outlive it.
    TerminalAccessible* accessibleOf(TerminalDisplay* display)
    {
        if (!TerminalAccessible::isActive())
            return nullptr;
        return dynamic_cast<TerminalAccessible*>(QAccessible::queryAccessibleInterface(display));
    }
} // namespace

void TerminalDisplay::reportAccessibleCaret()
{
    if (auto* accessible = accessibleOf(this))
        accessible->reportCaret();
}

void TerminalDisplay::resetAccessibleCaret()
{
    if (auto* accessible = accessibleOf(this))
        accessible->resetCaretGate();
}

vtbackend::FontDef TerminalDisplay::getFontDef()
{
    Require(_renderer);
    return getFontDefinition(*_renderer);
}

void TerminalDisplay::copyToClipboard(std::string_view data)
{
    if (QClipboard* clipboard = QGuiApplication::clipboard(); clipboard != nullptr)
        clipboard->setText(QString::fromUtf8(data.data(), static_cast<int>(data.size())));
}

void TerminalDisplay::inspect()
{
    // Ensure we're invoked on GUI thread when calling doDumpState().
    QMetaObject::invokeMethod(this, &TerminalDisplay::doDumpState, Qt::QueuedConnection);
}

void TerminalDisplay::doDumpState()
{
    // Arms the dump; paint() (render thread) services it on the next frame. NB: on the at-exit path
    // the screenshot half cannot yet complete offscreen (the deferred RHI readback needs scene-graph
    // frames the offscreen platform does not schedule) — see ROADMAP.md. The ordering + GL-gate
    // fixes make the SCREEN-STATE dump reach paint() correctly; the screenshot remains a known gap.
    _doDumpState = true;
}

QImage TerminalDisplay::screenshotImageFromBuffer(std::vector<uint8_t> const& rgbaBuffer,
                                                  vtbackend::ImageSize pixelSize)
{
    // The buffer arrives already normalized to a top-left-origin, tightly-packed RGBA8 image (the renderer's
    // deliverScreenshot() reverses the rows when the capture came off a Y-up framebuffer), so this wrapper
    // only adopts it. copy() detaches from the (transient) source buffer so the returned image stays valid
    // after it is freed.
    return QImage(rgbaBuffer.data(),
                  pixelSize.width.as<int>(),
                  pixelSize.height.as<int>(),
                  QImage::Format_RGBA8888_Premultiplied)
        .copy();
}

void TerminalDisplay::requestScreenshot(std::function<void(QImage)> onReady)
{
    if (!_renderTarget)
        return;

    // Deferred capture: the RHI renders the terminal into an offscreen texture and reads it back one frame
    // later. Wrap the caller's continuation so it receives a ready-to-use QImage. update() nudges the scene
    // graph to run a frame so the capture + readback actually happen — but both callers (paint(),
    // doDumpStateInternal()) run on the QSGRenderThread, where QQuickItem::update() must not be called
    // directly, so post it to the GUI thread and re-check liveness at dispatch (mirroring paint()).
    _renderTarget->scheduleScreenshot([onReady = std::move(onReady)](std::vector<uint8_t> const& rgbaBuffer,
                                                                     vtbackend::ImageSize pixelSize) {
        onReady(screenshotImageFromBuffer(rgbaBuffer, pixelSize));
    });
    post([this]() {
        if (window())
            update();
    });
}

void TerminalDisplay::doDumpStateInternal()
{
    // On the at-exit dump path the session must be terminated once the dump is complete. The screenshot is
    // now a *deferred* RHI readback (requestScreenshot only schedules the offscreen capture; the PNG lands a
    // frame or two later), so termination must be chained onto the screenshot's completion — terminating
    // eagerly here would tear the window down before the readback runs and screenshot.png would never be
    // written. terminateOnExit() is invoked from every path that does not reach that deferred callback (the
    // early-outs below and the no-renderer case in requestScreenshot); the callback owns termination on the
    // normal path.
    auto const terminateOnExit = [this] {
        if (_session->terminal().device().isClosed() && _session->app().dumpStateAtExit().has_value())
            _session->terminate();
    };

    // NB: no QOpenGLContext gate here. This runs inside paint() on the scene-graph render thread,
    // where rendering goes through the RHI (not a raw Qt QOpenGLContext), so the old
    // currentContext()/makeCurrent() check spuriously failed under the RHI render loop and skipped
    // the entire dump. The screen-state dump below needs no GL, and the screenshot uses the RHI
    // readback path (requestScreenshot), which schedules its own offscreen pass.
    Require(_session);
    Require(_renderer);

    // clang-format off
    auto const targetBaseDir = _session->app().dumpStateAtExit().value_or(crispy::app::instance()->localStateDir() / "dump");
    auto const workDirName = fs::path(std::format("contour-dump-{:%Y-%m-%d-%H-%M-%S}", chrono::system_clock::now()));
    auto const targetDir = targetBaseDir / workDirName;
    auto const latestDirName = fs::path("latest");
    // clang-format on

    fs::create_directories(targetDir);

    if (fs::exists(targetBaseDir / latestDirName))
        fs::remove(targetBaseDir / latestDirName);

    fs::create_symlink(workDirName, targetBaseDir / latestDirName);

    displayLog()("Dumping state into directory: {}", targetDir.generic_string());

    // TODO: The above should be done from the outside and the targetDir being passed into this call.
    // TODO: maybe zip this dir in the end.

    // TODO: use this file store for everything that needs to be dumped.
    {
        auto const screenStateDump = [&]() {
            auto os = std::stringstream {};
            terminal().currentScreen().inspect("Screen state dump.", os);
            _renderer->inspect(os);
            return os.str();
        }();

        std::cout << screenStateDump;

        auto const screenStateDumpFilePath = targetDir / "screen-state-dump.vt";
        auto fs = ofstream { screenStateDumpFilePath.string(), ios::trunc };
        fs << screenStateDump;
        fs.close();
    }

    enum class ImageBufferFormat : uint8_t
    {
        RGBA,
        RGB,
        Alpha
    };

    vtrasterizer::RenderTarget& renderTarget = _renderer->renderTarget();

    do
    {
        auto infoOpt = renderTarget.readAtlas();
        if (!infoOpt.has_value())
            break;

        vtrasterizer::AtlasTextureScreenshot const& info = infoOpt.value();
        auto const fileName = targetDir / "texture-atlas-rgba.png";
        displayLog()("Saving image {} to: {}", info.size, fileName.generic_string());

        QImage(info.buffer.data(),
               info.size.width.as<int>(),
               info.size.height.as<int>(),
               QImage::Format_RGBA8888)
            .save(QString::fromStdString(fileName.generic_string()));
    } while (0);

    auto screenshotFilePath = targetDir / "screenshot.png";
    displayLog()("Requesting screenshot for state dump: {}", screenshotFilePath.generic_string());

    // Deferred capture: the RHI reads the offscreen render back a frame or two later, so save it when it
    // arrives and only *then* terminate on the at-exit path — terminating now would close the window before
    // the readback completes and the PNG would never be written. If there is no renderer, requestScreenshot
    // never invokes the callback, so terminate here to avoid leaking the session on that path.
    if (!_renderTarget)
    {
        terminateOnExit();
        return;
    }
    requestScreenshot([screenshotFilePath, terminateOnExit](QImage const& image) {
        image.save(QString::fromStdString(screenshotFilePath.string()));
        terminateOnExit();
    });
}

void TerminalDisplay::resizeTerminalToDisplaySize()
{
    // Refit the grid to this item's current extent (same pixels, adjusted rows/columns and margin).
    Require(_renderer != nullptr);
    Require(_session != nullptr);

    applyResize(geometry::availableDevicePixels(width(), height(), contentScale()), *_session, *_renderer);
}

void TerminalDisplay::resizeWindow(vtbackend::Width newWidth, vtbackend::Height newHeight)
{
    Require(_session != nullptr);

    // CSI 4 t: pixel-size request for this pane's content area (device pixels). Routed through the
    // controller choke point: it resizes the WINDOW, and the grid follows from the resulting resize
    // event via the normal window->grid path. (The former direct applyResize() here resized the grid
    // WITHOUT the window — a guaranteed grid/window divergence.)
    if (auto* controller = windowController())
        controller->resizeWindowForContentPixels(*this, vtbackend::ImageSize { newWidth, newHeight });
}

void TerminalDisplay::resizeWindow(vtbackend::LineCount newLineCount, vtbackend::ColumnCount newColumnCount)
{
    Require(_session != nullptr);

    // The TUI app requests the usable area (what the PTY reports via TIOCSWINSZ),
    // not the total page size. Add back the status line height to get the total page size.
    auto requestedPageSize = terminal().totalPageSize();
    if (*newColumnCount)
        requestedPageSize.columns = newColumnCount;
    if (*newLineCount)
        requestedPageSize.lines = newLineCount + terminal().statusLineHeight();

    if (auto* controller = windowController())
        controller->resizeWindowForPage(*this, requestedPageSize);
}

void TerminalDisplay::recomputeGeometryAfterFontReconfig()
{
    // Re-derive the geometry that depends on the (now current) cell size after a font/DPI reconfiguration:
    // the terminal page size + render-surface margin (resizeTerminalToDisplaySize, which also drives the
    // resizeScreen()/SIGWINCH to the child) and the WM size hints. Callers must ensure the display is
    // fully alive (live _session, _renderer and window()).
    Require(_session && _renderer && window());
    resizeTerminalToDisplaySize();
    notifyCellGeometryChanged();
}

bool TerminalDisplay::applyStagedFontReconfigNow()
{
    // Apply a staged font/DPI reconfiguration synchronously, on the GUI thread, and re-derive geometry
    // against the resulting cell size in this same call — rather than deferring to a later painted frame.
    //
    // This is the single policy for ALL discrete font reconfigurations (size, family, DPI). Deferring to a
    // frame had three problems the synchronous path avoids: (1) the child process learned its new row/col
    // count one to two frames late (a TUI read of the terminal size right after the change saw stale
    // dimensions); (2) the intervening frame(s) rendered the new cell size against the previous page size,
    // a visible wrong-column-count flash; (3) it relied on a frame actually painting, so an exposed-but-
    // not-painting window (occluded, compositor withholding frames) could leave the change unapplied
    // indefinitely. Font reconfigurations are discrete user/config events, not the continuous
    // resize stream the render-thread deferral exists to protect, so applying them inline is appropriate.
    //
    // Safety: applyStagedReconfigDuringSetup() takes the renderer's _applyMutex, which renderImpl() also
    // holds across a whole frame, so this is mutually exclusive with an in-flight render-thread frame even
    // if the window has not yet observed losing exposure — it waits for that frame to finish reading the
    // grid metrics / atlas before mutating them. This is the same mechanism applyFontDPI() relied on.
    //
    // applyStagedReconfigDuringSetup() both applies the staged change and consumes the "font reconfig
    // applied" one-shot signal under _applyMutex, returning it. Consuming it there (not via a separate
    // consumeFontReconfigApplied() after the lock is released) is what prevents this GUI-thread path from
    // racing a render-thread frame for the signal.
    //
    // @return true if the cell size actually changed (a geometry recompute against it was performed). DPI
    //         changes that round to the same cell pixel size return false but still alter DPR-derived
    //         window geometry; applyFontDPI() refreshes the WM size hints itself for that case.
    auto const fontReconfigApplied = _renderer->applyStagedReconfigDuringSetup();
    if (fontReconfigApplied && _session && window())
    {
        // recomputeGeometryAfterFontReconfig() only *stages* the new geometry (page size, margin, render
        // surface) into the renderer's pending reconfig. Drain it synchronously too so it lands now —
        // including the resizeScreen()/SIGWINCH to the child — rather than waiting for a frame.
        recomputeGeometryAfterFontReconfig();
        (void) _renderer->applyStagedReconfigDuringSetup();
    }
    return fontReconfigApplied;
}

void TerminalDisplay::setFonts(vtrasterizer::FontDescriptions fontDescriptions)
{
    Require(_session != nullptr);
    Require(_renderTarget != nullptr);

    if (applyFontDescription(fontDPI(), *_renderer, std::move(fontDescriptions)))
    {
        // The font change is only *staged* (see applyFontDescription). Apply it synchronously and
        // re-derive geometry against the new cell size now; doing the recompute without the apply would
        // use the *stale* cell size.
        applyStagedFontReconfigNow();
        // Rebuild the ReGIS text rasterizer against the new font so ReGIS text follows the reload.
        updateReGISTextRasterizer();
        // logDisplayInfo();
    }
}

void TerminalDisplay::updateReGISTextRasterizer()
{
    if (!_session)
        return;
    auto const dpi = fontDPI();
    auto const font = sanitizeFontDescription(profile().fonts.value(), dpi).regular;
    // Rebuild only when the font or DPI actually changed -- a fresh shaper is not free, and a tab
    // switch between same-font profiles should reuse the existing instance.
    if (!_regisTextRasterizer || _regisTextRasterizerFont != font || _regisTextRasterizerDpi != dpi)
    {
        _regisTextRasterizer = std::make_shared<vtrasterizer::ReGISFontRasterizer>(dpi, font);
        _regisTextRasterizerFont = font;
        _regisTextRasterizerDpi = dpi;
    }
    _session->terminal().setReGISTextRasterizer(_regisTextRasterizer);
}

bool TerminalDisplay::setFontSize(text::font_size newFontSize)
{
    Require(_renderer != nullptr);

    displayLog()("Setting display font size and recompute metrics: {}pt", newFontSize.pt);

    if (!_renderer->setFontSize(newFontSize))
        return false;

    // Font zoom is window/pane-preserving: neither the QWindow nor this pane changes size. The font
    // change is only *staged*; apply it synchronously so the grid reflows in place against this item's
    // fixed pixel extent (applyStagedFontReconfigNow drives that, unconditionally — the grid ALWAYS fits
    // the item), re-deriving the page size against the new cell size. A larger font therefore yields
    // fewer columns/rows and a smaller font more; that reflow drives resizeScreen()/SIGWINCH so the child
    // learns its new size. This is deliberate: the window is never resized to restore the old cell count.
    // (Recomputing without the apply would use the *stale* cell size.)
    applyStagedFontReconfigNow();

    // Report whether the change actually took: the render-thread apply catches and swallows font-load
    // failures (keeping the previous font), so the caller must not record a size the renderer never
    // loaded. font_size has no operator==; compare the point size the apply published against the request.
    // The request is the exact value just staged (no arithmetic in between), so an exact compare is
    // correct: equal means the apply loaded it, unequal means it was swallowed.
    return _renderer->fontDescriptions().size.pt == newFontSize.pt;
}

void TerminalDisplay::setMouseCursorShape(MouseCursorShape newCursorShape)
{
    if (auto const qtShape = toQtMouseShape(newCursorShape); qtShape != cursor().shape())
        setCursor(qtShape);
}

void TerminalDisplay::setWindowFullScreen()
{
    if (auto* controller = windowController())
        controller->setWindowFullScreen(*this);
}

void TerminalDisplay::setWindowMaximized()
{
    if (auto* controller = windowController())
        controller->setWindowMaximized(*this);
}

void TerminalDisplay::setWindowNormal()
{
    if (auto* controller = windowController())
        controller->setWindowNormal(*this);
}

void TerminalDisplay::setBlurBehind(bool enable)
{
    BlurBehind::setEnabled(window(), enable);
}

void TerminalDisplay::toggleFullScreen()
{
    if (auto* controller = windowController())
        controller->toggleFullScreen(*this);
}

void TerminalDisplay::setTabBarVisibility(config::TabBarVisibility mode)
{
    if (auto* controller = windowController())
        controller->setTabBarVisibility(mode);
}

void TerminalDisplay::setTabBarPosition(config::TabBarPosition position)
{
    if (auto* controller = windowController())
        controller->setTabBarPosition(position);
}

void TerminalDisplay::toggleTitleBar()
{
    // Title-bar visibility is WINDOW state: it lives on the WindowController (the window authority),
    // so a toggle from any pane flips the whole window's decoration and survives pane-focus changes
    // and tab switches (per-display storage silently reverted it on the next focus).
    if (auto* controller = windowController())
        controller->toggleTitleBar();
}

void TerminalDisplay::toggleInputMethodEditorHandling()
{
    auto const enabled = !static_cast<bool>(flags() & Flag::ItemAcceptsInputMethod);
    displayLog()("{} IME (input method editor) handling", enabled ? "Enabling" : "Disabling");
    setFlag(Flag::ItemAcceptsInputMethod, enabled);
}

void TerminalDisplay::setHyperlinkDecoration(vtrasterizer::Decorator normal, vtrasterizer::Decorator hover)
{
    _renderer->setHyperlinkDecoration(normal, hover);
}
// }}}

// {{{ TerminalDisplay: terminal events
void TerminalDisplay::scheduleRedraw()
{
    // Reached synchronously from the backend/parser thread (TerminalSession::screenUpdated ->
    // scheduleRedraw) while a VT sequence is processed. During a split, the GUI thread hands this session
    // from the hidden single-pane display to the new pane display: attachDisplay() calls the old display's
    // releaseSession(), which nulls its _session before the session's _display is repointed. In that window
    // the parser thread can land here on a display whose _session is already null. A released display has
    // nothing to redraw, so bail out. hasSession() reads _session once; the pointed-to session (when
    // non-null) owns the very thread calling us and is joined in its own destructor, so it cannot be freed
    // mid-call — the check is sufficient without a lock.
    if (!hasSession())
        return;

    auto const currentHistoryLineCount = terminal().currentScreen().historyLineCount();
    if (currentHistoryLineCount != _lastHistoryLineCount)
    {
        // emit historyLineCountChanged(unbox<int>(currentHistoryLineCount));
        _lastHistoryLineCount = currentHistoryLineCount;
    }

    // QCoreApplication::postEvent(this, new QEvent(QEvent::UpdateRequest));
    // post() queues the lambda to run LATER on the GUI event loop, so window() must be re-checked at the
    // point of USE, not only here at scheduling time: closing a split pane unparents this item (window() ->
    // null) between the post and its dispatch, and window()->update() on the null window then segfaults
    // (member call on null QQuickWindow). Guarding only the outer post() is a check-then-use-later race.
    post([this]() {
        if (window())
            window()->update();
    });
}

void TerminalDisplay::renderBufferUpdated()
{
    scheduleRedraw();
}

void TerminalDisplay::closeDisplay()
{
    displayLog()("closeDisplay");
    emit terminated();
}

void TerminalDisplay::onSelectionCompleted()
{
    // Like scheduleRedraw(), this can be reached from the backend thread during a split-pane session
    // hand-off, when this display's _session was just nulled by releaseSession(). Extracting the selection
    // then would dereference the null session via terminal(); a released display owns no selection, so bail.
    if (!hasSession())
        return;

    if (QClipboard* clipboard = QGuiApplication::clipboard(); clipboard != nullptr)
    {
        string const text = terminal().extractSelectionText();
        clipboard->setText(QString::fromUtf8(text.c_str(), static_cast<int>(text.size())),
                           QClipboard::Selection);
    }
}

void TerminalDisplay::bufferChanged(vtbackend::ScreenType type)
{
    using Type = vtbackend::ScreenType;
    switch (type)
    {
        case Type::Primary: setCursor(Qt::IBeamCursor); break;
        case Type::Alternate: setCursor(Qt::ArrowCursor); break;
    }
    emit terminalBufferChanged(type);
    // scheduleRedraw();
}

void TerminalDisplay::discardImage(vtbackend::Image const& image)
{
    _renderer->discardImage(image);
}

// }}}

} // namespace contour::display
