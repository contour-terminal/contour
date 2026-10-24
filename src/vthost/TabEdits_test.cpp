// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <vthost/TabEdits.hpp>
#include <vtworkspace/Pane.hpp>
#include <vtworkspace/Tab.hpp>

using vthost::applyTabEdit;
using vthost::TabEditEffect;
using vthost::TabEditError;
using vthost::proto::EditTab;
using vthost::proto::TabEditKind;

namespace
{

/// Counts every model event, so a case can tell whether an edit fired any.
struct CountingEvents final: vtworkspace::ModelEvents
{
    int count = 0;

    void tabAdded(vtworkspace::WindowId, vtworkspace::TabId, int) override { ++count; }
    void tabClosed(vtworkspace::WindowId, vtworkspace::TabId, int) override { ++count; }
    void tabMoved(vtworkspace::WindowId, vtworkspace::TabId, int, int) override { ++count; }
    void activeTabChanged(vtworkspace::WindowId, vtworkspace::TabId, int) override { ++count; }
    void paneSplit(vtworkspace::TabId, vtworkspace::PaneId, vtworkspace::PaneId) override { ++count; }
    void paneClosed(vtworkspace::TabId, vtworkspace::PaneId, vtworkspace::PaneId) override { ++count; }
    void activePaneChanged(vtworkspace::TabId, vtworkspace::PaneId) override { ++count; }
    void paneRatioChanged(vtworkspace::TabId, vtworkspace::PaneId, double) override { ++count; }
    void paneZoomChanged(vtworkspace::TabId, std::optional<vtworkspace::PaneId>) override { ++count; }
    void tabTitleChanged(vtworkspace::TabId) override { ++count; }
    void tabColorChanged(vtworkspace::TabId) override { ++count; }
};

/// A window of two tabs: the first split in two (sessions 1 and 2, the second pane active), the second
/// holding session 3 and active.
struct TwoPaneTab
{
    CountingEvents events;
    uint64_t nextSession = 1;
    vtworkspace::SessionModel model { events, [this] { return vtworkspace::SessionId { nextSession++ }; } };
    vtworkspace::WindowId window = model.createWindow()->id();
    vtworkspace::Tab* first = model.createTab(window);
    vtworkspace::Pane* second = model.splitActivePane(first->id(), vtworkspace::SplitState::Horizontal);
    vtworkspace::Tab* other = model.createTab(window);

    TwoPaneTab() { model.setActivePane(first->id(), *paneOf(2)); }

    /// @return The id of the pane hosting @p session.
    [[nodiscard]] std::optional<vtworkspace::PaneId> paneOf(uint64_t session) const
    {
        auto const [owner, tab, leaf] = model.findSessionLeaf(vtworkspace::SessionId { session });
        return leaf != nullptr ? std::optional { leaf->id() } : std::nullopt;
    }
};

/// One kind of edit: what puts the model in the state the edit changes, the edit, and what it then holds.
struct EditRow
{
    std::string name;
    EditTab edit;
    std::function<void(TwoPaneTab&)> prepare;
    std::function<bool(TwoPaneTab&)> holds;
};

} // namespace

TEST_CASE("each tab edit changes the model once and is idempotent", "[vthost][tab-edits]")
{
    auto const nothing = [](TwoPaneTab&) {
    };
    auto const rows = std::vector<EditRow> {
        { .name = "ActivatePane",
          .edit = { .session = 1, .kind = TabEditKind::ActivatePane },
          .prepare = nothing,
          .holds = [](TwoPaneTab& t) { return t.first->activePane()->id() == t.paneOf(1); } },
        { .name = "Zoom",
          .edit = { .session = 1, .kind = TabEditKind::Zoom },
          .prepare = nothing,
          .holds = [](TwoPaneTab& t) { return t.first->zoomedLeafId() == t.paneOf(1); } },
        { .name = "Unzoom",
          .edit = { .session = 1, .kind = TabEditKind::Unzoom },
          .prepare = [](TwoPaneTab& t) { t.model.toggleActivePaneZoom(t.first->id()); },
          .holds = [](TwoPaneTab& t) { return !t.first->isZoomed(); } },
        { .name = "SetTitle",
          .edit = { .session = 1, .kind = TabEditKind::SetTitle, .title = "build" },
          .prepare = nothing,
          .holds = [](TwoPaneTab& t) { return t.first->runtimeTitle() == "build"; } },
        { .name = "ClearTitle",
          .edit = { .session = 1, .kind = TabEditKind::ClearTitle },
          .prepare = [](TwoPaneTab& t) { t.model.setTabTitle(t.first->id(), "old"); },
          .holds = [](TwoPaneTab& t) { return !t.first->runtimeTitle(); } },
        { .name = "SetColor",
          .edit = { .session = 1, .kind = TabEditKind::SetColor, .color = 0x112233 },
          .prepare = nothing,
          .holds =
              [](TwoPaneTab& t) {
                  return t.first->color(vtworkspace::TabColorSource::User)
                         == vtbackend::RGBColor { 0x112233U };
              } },
        { .name = "ClearColor",
          .edit = { .session = 1, .kind = TabEditKind::ClearColor },
          .prepare =
              [](TwoPaneTab& t) {
                  t.model.setTabColor(
                      t.first->id(), vtworkspace::TabColorSource::User, vtbackend::RGBColor { 0x445566U });
              },
          .holds = [](TwoPaneTab& t) { return !t.first->color(vtworkspace::TabColorSource::User); } },
        { .name = "ActivateTab",
          .edit = { .session = 1, .kind = TabEditKind::ActivateTab },
          .prepare = nothing,
          .holds = [](TwoPaneTab& t) { return t.model.window(t.window)->activeTab() == t.first; } },
    };
    CHECK(rows.size() == 8); // every TabEditKind

    for (auto const& row: rows)
    {
        INFO(row.name);
        auto t = TwoPaneTab {};
        row.prepare(t);
        REQUIRE_FALSE(row.holds(t));
        t.events.count = 0;

        CHECK(applyTabEdit(t.model, row.edit) == TabEditEffect::Changed);
        CHECK(row.holds(t));
        CHECK(t.events.count > 0);

        auto const eventsAfterFirst = t.events.count;
        CHECK(applyTabEdit(t.model, row.edit) == TabEditEffect::Unchanged);
        CHECK(row.holds(t));
        CHECK(t.events.count == eventsAfterFirst);
    }
}

TEST_CASE("an edit naming an unknown session or kind is refused", "[vthost][tab-edits]")
{
    auto t = TwoPaneTab {};
    CHECK(applyTabEdit(t.model, { .session = 999, .kind = TabEditKind::SetTitle }).error()
          == TabEditError::UnknownSession);
    CHECK(applyTabEdit(t.model, { .session = 1, .kind = static_cast<TabEditKind>(0xEE) }).error()
          == TabEditError::UnknownKind);
    CHECK_FALSE(t.first->runtimeTitle());
}

TEST_CASE("a zoom on another pane of a zoomed tab moves the zoom", "[vthost][tab-edits]")
{
    auto t = TwoPaneTab {};
    t.model.toggleActivePaneZoom(t.first->id()); // zoomed on session 2
    CHECK(applyTabEdit(t.model, { .session = 1, .kind = TabEditKind::Zoom }) == TabEditEffect::Changed);
    CHECK(t.first->zoomedLeafId() == t.paneOf(1));
}
