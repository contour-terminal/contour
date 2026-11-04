// SPDX-License-Identifier: Apache-2.0
#include <vthost/ObjectRegistry.hpp>

#include <algorithm>
#include <ranges>
#include <utility>

namespace vthost
{

using vtcontrol::ObjectKind;
using vtcontrol::ObjectRef;
using vtcontrol::Uuid;

ObjectRegistry::ObjectRegistry(std::shared_ptr<vtcontrol::UuidSource> uuids): _uuids { std::move(uuids) }
{
}

Uuid ObjectRegistry::mint()
{
    return _uuids->next();
}

ObjectRegistry::Table const& ObjectRegistry::tableOf(ObjectKind kind) const noexcept
{
    switch (kind)
    {
        case ObjectKind::Window: return _windows;
        case ObjectKind::Tab: return _tabs;
        case ObjectKind::Pane: break;
    }
    return _panes;
}

void ObjectRegistry::bindIn(Table& table, ObjectKind kind, uint64_t id, Uuid uuid)
{
    forgetIn(table, kind, id);
    // A UUID names one object: if it was bound to another id, that id loses it.
    if (auto const other = table.byUuid.find(uuid); other != table.byUuid.end())
        table.byId.erase(other->second);
    table.byId.insert_or_assign(id, uuid);
    table.byUuid.insert_or_assign(uuid, id);
}

void ObjectRegistry::forgetIn(Table& table, ObjectKind kind, uint64_t id)
{
    auto const known = table.byId.find(id);
    if (known == table.byId.end())
        return;
    _labels.erase(ObjectRef { .kind = kind, .uuid = known->second });
    table.byUuid.erase(known->second);
    table.byId.erase(known);
}

std::optional<Uuid> ObjectRegistry::uuidIn(Table const& table, uint64_t id)
{
    if (auto const known = table.byId.find(id); known != table.byId.end())
        return known->second;
    return std::nullopt;
}

void ObjectRegistry::bind(vtworkspace::WindowId id, Uuid uuid)
{
    bindIn(_windows, ObjectKind::Window, id.value, uuid);
}

void ObjectRegistry::bind(vtworkspace::TabId id, Uuid uuid)
{
    bindIn(_tabs, ObjectKind::Tab, id.value, uuid);
}

void ObjectRegistry::bind(vtworkspace::SessionId id, Uuid uuid)
{
    bindIn(_panes, ObjectKind::Pane, id.value, uuid);
}

void ObjectRegistry::forget(vtworkspace::WindowId id)
{
    forgetIn(_windows, ObjectKind::Window, id.value);
}

void ObjectRegistry::forget(vtworkspace::TabId id)
{
    forgetIn(_tabs, ObjectKind::Tab, id.value);
}

void ObjectRegistry::forget(vtworkspace::SessionId id)
{
    forgetIn(_panes, ObjectKind::Pane, id.value);
}

std::optional<Uuid> ObjectRegistry::uuidOf(vtworkspace::WindowId id) const
{
    return uuidIn(_windows, id.value);
}

std::optional<Uuid> ObjectRegistry::uuidOf(vtworkspace::TabId id) const
{
    return uuidIn(_tabs, id.value);
}

std::optional<Uuid> ObjectRegistry::uuidOf(vtworkspace::SessionId id) const
{
    return uuidIn(_panes, id.value);
}

std::optional<ObjectRegistry::Handle> ObjectRegistry::find(ObjectRef const& ref) const
{
    auto const& table = tableOf(ref.kind);
    auto const known = table.byUuid.find(ref.uuid);
    if (known == table.byUuid.end())
        return std::nullopt;
    switch (ref.kind)
    {
        case ObjectKind::Window: return Handle { vtworkspace::WindowId { known->second } };
        case ObjectKind::Tab: return Handle { vtworkspace::TabId { known->second } };
        case ObjectKind::Pane: break;
    }
    return Handle { vtworkspace::SessionId { known->second } };
}

void ObjectRegistry::setLabel(ObjectRef const& ref, std::string label)
{
    if (!find(ref))
        return;
    if (label.empty())
        _labels.erase(ref);
    else
        _labels.insert_or_assign(ref, std::move(label));
}

std::string_view ObjectRegistry::labelOf(ObjectRef const& ref) const
{
    if (auto const known = _labels.find(ref); known != _labels.end())
        return known->second;
    return {};
}

namespace
{
    /// @return Every object @p byId binds, by id ascending.
    [[nodiscard]] std::vector<BoundObject> boundIn(std::unordered_map<uint64_t, Uuid> const& byId)
    {
        auto bound =
            std::ranges::to<std::vector>(byId | std::views::transform([](auto const& entry) {
                                             return BoundObject { .id = entry.first, .uuid = entry.second };
                                         }));
        std::ranges::sort(bound, {}, &BoundObject::id);
        return bound;
    }
} // namespace

ObjectRegistrySnapshot ObjectRegistry::snapshot() const
{
    return ObjectRegistrySnapshot {
        .windows = boundIn(_windows.byId),
        .tabs = boundIn(_tabs.byId),
        .panes = boundIn(_panes.byId),
        .labels = std::ranges::to<std::vector>(_labels | std::views::transform([](auto const& entry) {
                                                   return LabeledObject { .ref = entry.first,
                                                                          .label = entry.second };
                                               })),
    };
}

void ObjectRegistry::restore(ObjectRegistrySnapshot const& snapshot)
{
    _windows = {};
    _tabs = {};
    _panes = {};
    _labels.clear();
    for (auto const& window: snapshot.windows)
        bind(vtworkspace::WindowId { window.id }, window.uuid);
    for (auto const& tab: snapshot.tabs)
        bind(vtworkspace::TabId { tab.id }, tab.uuid);
    for (auto const& pane: snapshot.panes)
        bind(vtworkspace::SessionId { pane.id }, pane.uuid);
    // After the binds: a label of an object not bound is ignored.
    for (auto const& labeled: snapshot.labels)
        setLabel(labeled.ref, labeled.label);
}

} // namespace vthost
