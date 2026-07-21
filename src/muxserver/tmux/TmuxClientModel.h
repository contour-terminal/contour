// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `TmuxClientModel` — the client-side window/pane mirror behind a TmuxGateway.
///
/// Consumes gateway notifications and maintains, per remote pane, a REAL
/// `vtbackend::Terminal` fed by the raw %output byte stream (the tmux server
/// forwards bytes; the client emulates — images and OSC 66 parse natively
/// while attached). History arrives once per pane via `capture-pane -peqJ`
/// replay: text + SGR only, tmux's own inherited limitation. Layout strings
/// ingest through parseLayout/collapseToBinary into a vtmux-shaped tree a
/// frontend can realize.

#include <vtbackend/Terminal.h>

#include <vtpty/MockPty.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <muxserver/tmux/LayoutString.h>
#include <muxserver/tmux/PaneSink.h>
#include <muxserver/tmux/TmuxGateway.h>

namespace muxserver::tmux
{

/// One mirrored remote pane: a replay terminal the frontend renders from —
/// the default PaneSink when no factory is injected.
class PaneView final: public PaneSink
{
  public:
    /// @param columns The pane's width in cells (from the layout leaf).
    /// @param lines The pane's height in cells.
    PaneView(int columns, int lines);

    /// Feeds raw remote bytes (live %output or capture-pane replay).
    void feed(std::string_view bytes) override;

    /// Resizes the replay terminal to a new layout extent.
    void resize(int columns, int lines) override;

    [[nodiscard]] vtbackend::Terminal& terminal() noexcept { return *_terminal; }
    [[nodiscard]] vtbackend::Terminal const& terminal() const noexcept { return *_terminal; }

    /// The visible page as plain text — the test/debug projection.
    [[nodiscard]] std::string pageText() const;

  private:
    vtbackend::Terminal::NullEvents _events; ///< Must outlive _terminal.
    std::unique_ptr<vtbackend::Terminal> _terminal;
};

/// The mirrored state of one remote window.
struct WindowView
{
    std::string name;
    std::string layout;                 ///< The last ingested layout string.
    std::unique_ptr<BinaryLayout> tree; ///< Its binary split tree (vtmux-shaped).
    std::vector<uint64_t> panes;        ///< Leaf pane ids, layout order.
};

/// Observer of the mirrored model's structure — what a frontend realizes as
/// tabs and splits. All callbacks fire on the model's (reactor) thread.
class TmuxModelEvents
{
  public:
    virtual ~TmuxModelEvents() = default;

    virtual void windowAdded(uint64_t /*window*/) {}
    virtual void windowClosed(uint64_t /*window*/) {}
    virtual void windowRenamed(uint64_t /*window*/, std::string const& /*name*/) {}
    virtual void paneAdded(uint64_t /*window*/, uint64_t /*pane*/, int /*columns*/, int /*lines*/) {}
    virtual void paneRemoved(uint64_t /*window*/, uint64_t /*pane*/) {}
    /// The window's split tree changed shape or ratios (after pane add/remove
    /// callbacks of the same ingest fired).
    virtual void layoutTreeChanged(uint64_t /*window*/) {}
    virtual void panePaused(uint64_t /*pane*/, bool /*paused*/) {}
    /// The gateway ended (%exit or disconnect).
    virtual void exited(std::string const& /*reason*/) {}
};

/// The gateway-driven client model: windows, panes, replay terminals.
class TmuxClientModel final: public GatewayEvents
{
  public:
    /// Binds the gateway used for history replay and window enumeration.
    /// (Constructed first, since the gateway's constructor takes the events.)
    void bind(TmuxGateway& gateway) noexcept { _gateway = &gateway; }

    /// Installs @p factory as the backing-sink source for NEW panes; existing
    /// panes keep their sink. Unset, panes are backed by replay PaneViews.
    void setPaneSinkFactory(PaneSinkFactory factory) { _sinkFactory = std::move(factory); }

    /// Registers @p observer for structural changes. Not owned.
    void subscribe(TmuxModelEvents* observer) { _observers.push_back(observer); }

    /// Removes @p observer. Idempotent.
    void unsubscribe(TmuxModelEvents* observer) { std::erase(_observers, observer); }

    // GatewayEvents — notifications become model mutations.
    void outputReceived(uint64_t pane, std::string_view bytes) override;
    void layoutChanged(uint64_t window, std::string_view layout) override;
    void windowAdded(uint64_t window) override;
    void windowClosed(uint64_t window) override;
    void windowRenamed(uint64_t window, std::string_view name) override;
    void sessionChanged(uint64_t session, std::string_view name) override;
    void panePaused(uint64_t pane, bool paused) override;
    void exited(std::string_view reason) override;

    [[nodiscard]] std::map<uint64_t, WindowView> const& windows() const noexcept { return _windows; }

    /// @return The replay view backing @p id, or nullptr — for a pane that
    ///         does not exist OR one backed by an injected custom sink.
    [[nodiscard]] PaneView* pane(uint64_t id) noexcept;
    [[nodiscard]] std::size_t paneCount() const noexcept { return _panes.size(); }

  private:
    /// Applies @p layout to @p window: parses, rebuilds the tree, creates or
    /// resizes the leaf panes (new panes get a capture-pane history replay).
    void ingestLayout(uint64_t window, std::string_view layout);

    /// Requests `capture-pane -peqJ` for @p pane and replays the body.
    void replayHistory(uint64_t pane);

    /// One tracked pane plus its replay bookkeeping: live %output arriving
    /// BEFORE the history replay completed is buffered so the replayed
    /// history never lands on top of newer bytes.
    struct PaneEntry
    {
        std::unique_ptr<PaneSink> sink;
        bool replayed = false;
        std::string pendingOutput;
    };

    TmuxGateway* _gateway = nullptr;
    PaneSinkFactory _sinkFactory;
    std::vector<TmuxModelEvents*> _observers;
    std::map<uint64_t, WindowView> _windows;
    std::map<uint64_t, PaneEntry> _panes;
};

} // namespace muxserver::tmux
