// SPDX-License-Identifier: Apache-2.0
#include <vthost/TabEdits.hpp>

#include <vtbackend/core/Color.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <string>

#include <vtworkspace/Pane.hpp>
#include <vtworkspace/Tab.hpp>

namespace vthost
{

namespace
{
    using vtworkspace::SessionModel;
    using vtworkspace::TabColorSource;

    /// The pane an edit names, and the tab and window that hold it.
    struct EditTarget
    {
        vtworkspace::Window& window;
        vtworkspace::Tab& tab;
        vtworkspace::Pane& leaf;
    };

    /// What one kind of edit does: compares, and mutates only when the model differs.
    using TabEditFn = TabEditEffect (*)(SessionModel& model,
                                        EditTarget const& target,
                                        proto::EditTab const& edit);

    struct TabEditRow
    {
        proto::TabEditKind kind;
        TabEditFn apply;
    };

    [[nodiscard]] TabEditEffect activatePane(SessionModel& model,
                                             EditTarget const& target,
                                             proto::EditTab const&)
    {
        if (target.tab.activePane() == &target.leaf)
            return TabEditEffect::Unchanged;
        model.setActivePane(target.tab.id(), target.leaf.id());
        return TabEditEffect::Changed;
    }

    [[nodiscard]] TabEditEffect zoom(SessionModel& model,
                                     EditTarget const& target,
                                     proto::EditTab const& edit)
    {
        if (target.tab.zoomedLeafId() == target.leaf.id())
            return TabEditEffect::Unchanged;
        // Zoom follows focus: activating the pane of a tab zoomed on another moves the zoom to it.
        std::ignore = activatePane(model, target, edit);
        if (!target.tab.isZoomed())
            model.toggleActivePaneZoom(target.tab.id());
        return TabEditEffect::Changed;
    }

    [[nodiscard]] TabEditEffect unzoom(SessionModel& model, EditTarget const& target, proto::EditTab const&)
    {
        if (!target.tab.isZoomed())
            return TabEditEffect::Unchanged;
        model.toggleActivePaneZoom(target.tab.id());
        return TabEditEffect::Changed;
    }

    [[nodiscard]] TabEditEffect clearTitle(SessionModel& model,
                                           EditTarget const& target,
                                           proto::EditTab const&)
    {
        if (!target.tab.runtimeTitle())
            return TabEditEffect::Unchanged;
        model.resetTabTitle(target.tab.id());
        return TabEditEffect::Changed;
    }

    [[nodiscard]] TabEditEffect setTitle(SessionModel& model,
                                         EditTarget const& target,
                                         proto::EditTab const& edit)
    {
        // The model reads a blank title as a reset, and so does the comparison.
        if (std::ranges::all_of(edit.title, [](unsigned char c) { return std::isspace(c) != 0; }))
            return clearTitle(model, target, edit);
        if (target.tab.runtimeTitle() == edit.title)
            return TabEditEffect::Unchanged;
        model.setTabTitle(target.tab.id(), edit.title);
        return TabEditEffect::Changed;
    }

    [[nodiscard]] TabEditEffect setColor(SessionModel& model,
                                         EditTarget const& target,
                                         proto::EditTab const& edit)
    {
        auto const color = vtbackend::RGBColor { edit.color };
        if (target.tab.color(TabColorSource::User) == color)
            return TabEditEffect::Unchanged;
        model.setTabColor(target.tab.id(), TabColorSource::User, color);
        return TabEditEffect::Changed;
    }

    [[nodiscard]] TabEditEffect clearColor(SessionModel& model,
                                           EditTarget const& target,
                                           proto::EditTab const&)
    {
        if (!target.tab.color(TabColorSource::User))
            return TabEditEffect::Unchanged;
        model.resetTabColor(target.tab.id(), TabColorSource::User);
        return TabEditEffect::Changed;
    }

    [[nodiscard]] TabEditEffect activateTab(SessionModel& model,
                                            EditTarget const& target,
                                            proto::EditTab const&)
    {
        if (target.window.activeTab() == &target.tab)
            return TabEditEffect::Unchanged;
        model.activateTab(target.window.id(), target.tab.id());
        return TabEditEffect::Changed;
    }

    constexpr auto TabEditTable = std::array {
        TabEditRow { .kind = proto::TabEditKind::ActivatePane, .apply = &activatePane },
        TabEditRow { .kind = proto::TabEditKind::Zoom, .apply = &zoom },
        TabEditRow { .kind = proto::TabEditKind::Unzoom, .apply = &unzoom },
        TabEditRow { .kind = proto::TabEditKind::SetTitle, .apply = &setTitle },
        TabEditRow { .kind = proto::TabEditKind::ClearTitle, .apply = &clearTitle },
        TabEditRow { .kind = proto::TabEditKind::SetColor, .apply = &setColor },
        TabEditRow { .kind = proto::TabEditKind::ClearColor, .apply = &clearColor },
        TabEditRow { .kind = proto::TabEditKind::ActivateTab, .apply = &activateTab },
    };
} // namespace

std::expected<TabEditEffect, TabEditError> applyTabEdit(SessionModel& model, proto::EditTab const& edit)
{
    auto const [window, tab, leaf] = model.findSessionLeaf(vtworkspace::SessionId { edit.session });
    if (window == nullptr || tab == nullptr || leaf == nullptr)
        return std::unexpected { TabEditError::UnknownSession };
    auto const row = std::ranges::find(TabEditTable, edit.kind, &TabEditRow::kind);
    if (row == TabEditTable.end())
        return std::unexpected { TabEditError::UnknownKind };
    return row->apply(model, EditTarget { .window = *window, .tab = *tab, .leaf = *leaf }, edit);
}

} // namespace vthost
