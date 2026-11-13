// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The UUIDs and labels of the daemon's workspaces, tabs and panes.
///
/// The control API names objects by UUID, never by the `vtworkspace` ids, which are small integers that
/// restart with every daemon. An API pane is one hosted session, so a pane's UUID is keyed by its
/// `SessionId`: it follows the shell, not the `PaneId` tree node, which changes under swaps.

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include <vtcontrol/ObjectRef.hpp>
#include <vtcontrol/Uuid.hpp>
#include <vtworkspace/Primitives.hpp>

namespace vthost
{

/// The ids and labels a restored or pre-named object comes back with, instead of freshly minted ones.
struct PresetObjectIds
{
    vtcontrol::Uuid pane {};                     ///< The pane's UUID.
    std::optional<vtcontrol::Uuid> tab {};       ///< The tab's, when the spawn creates one.
    std::optional<vtcontrol::Uuid> workspace {}; ///< The workspace's, when the spawn creates one.
    std::string paneLabel {};                    ///< The pane's label; empty for none.
    std::string tabLabel {};                     ///< The tab's label; empty for none.
    std::string workspaceLabel {};               ///< The workspace's label; empty for none.
};

/// One workspace, tab or pane of the object registry, with the UUID it is known by.
struct BoundObject
{
    uint64_t id = 0;         ///< The model's id (WorkspaceId, TabId or SessionId, by the list it sits in).
    vtcontrol::Uuid uuid {}; ///< Its UUID.

    /// Lists the fields for the handover codec.
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.id);
        visitor(2, self.uuid);
    }
};

/// A label an object carries.
struct LabeledObject
{
    vtcontrol::ObjectRef ref {}; ///< The object.
    std::string label;           ///< Its label.

    /// Lists the fields for the handover codec.
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.ref);
        visitor(2, self.label);
    }
};

/// The UUIDs and labels of the daemon's workspaces, tabs and panes, as plain data: what crosses a live
/// upgrade, so that a client holding a reference from before it still finds its object afterwards.
struct ObjectRegistrySnapshot
{
    std::vector<BoundObject> workspaces; ///< Workspaces by WorkspaceId, ascending.
    std::vector<BoundObject> tabs;       ///< Tabs by TabId, ascending.
    std::vector<BoundObject> panes;      ///< Panes by SessionId, ascending.
    std::vector<LabeledObject> labels;   ///< Every label, ordered by object.

    /// Lists the fields for the handover codec.
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.workspaces);
        visitor(2, self.tabs);
        visitor(3, self.panes);
        visitor(4, self.labels);
    }
};

/// Maps the model's ids to UUIDs and back, and keeps the labels. Loop thread only.
///
/// UUIDs are never reused: they come from a monotonic source, and forgetting an object frees nothing for
/// the next one.
class ObjectRegistry
{
  public:
    /// Any of the model's handles.
    using Handle = std::variant<vtworkspace::WorkspaceId, vtworkspace::TabId, vtworkspace::SessionId>;

    /// Told that an object's label changed: the object, and its label now (empty when it was cleared).
    using LabelObserver = std::function<void(vtcontrol::ObjectRef const&, std::string_view)>;

  private:
    struct LabelObservers;

  public:
    /// Keeps a label observer subscribed for its own lifetime. It may outlive the registry, and the registry
    /// may move: the observers live apart from both.
    class Subscription
    {
      public:
        Subscription(Subscription const&) = delete;
        Subscription& operator=(Subscription const&) = delete;
        Subscription(Subscription&& other) noexcept;
        Subscription& operator=(Subscription&& other) noexcept;
        ~Subscription();

      private:
        friend class ObjectRegistry;
        Subscription(std::weak_ptr<LabelObservers> observers, uint64_t id) noexcept;
        void unsubscribe() noexcept;

        std::weak_ptr<LabelObservers> _observers;
        uint64_t _id = 0;
    };

    /// @param uuids Where fresh UUIDs come from.
    explicit ObjectRegistry(std::shared_ptr<vtcontrol::UuidSource> uuids);

    // Not copyable: a copy would share the label observers of the original.
    ObjectRegistry(ObjectRegistry const&) = delete;
    ObjectRegistry& operator=(ObjectRegistry const&) = delete;

    /// Moves are noexcept, so a moved-from registry holds nothing at all -- not even its observer list or its
    /// UUID source. It may only be destroyed or assigned to; any other call on it is undefined.
    ObjectRegistry(ObjectRegistry&&) noexcept = default;
    /// @copydoc ObjectRegistry(ObjectRegistry&&)
    ObjectRegistry& operator=(ObjectRegistry&&) noexcept = default;
    ~ObjectRegistry() = default;

    /// @return A fresh UUID, bound to nothing yet.
    [[nodiscard]] vtcontrol::Uuid mint();

    /// Names an object. Binding again replaces what it was bound to.
    /// @param id The model's id.
    /// @param uuid Its UUID.
    /// @{
    void bind(vtworkspace::WorkspaceId id, vtcontrol::Uuid uuid);
    void bind(vtworkspace::TabId id, vtcontrol::Uuid uuid);
    void bind(vtworkspace::SessionId id, vtcontrol::Uuid uuid);
    /// @}

    /// Drops an object and its label; an unknown one is a no-op.
    /// @param id The model's id.
    /// @{
    void forget(vtworkspace::WorkspaceId id);
    void forget(vtworkspace::TabId id);
    void forget(vtworkspace::SessionId id);
    /// @}

    /// @param id The model's id.
    /// @return Its UUID, or nothing when it is not bound.
    /// @{
    [[nodiscard]] std::optional<vtcontrol::Uuid> uuidOf(vtworkspace::WorkspaceId id) const;
    [[nodiscard]] std::optional<vtcontrol::Uuid> uuidOf(vtworkspace::TabId id) const;
    [[nodiscard]] std::optional<vtcontrol::Uuid> uuidOf(vtworkspace::SessionId id) const;
    /// @}

    /// @param ref A workspace, tab or pane reference.
    /// @return The model's handle for it, or nothing when it is gone or never was.
    [[nodiscard]] std::optional<Handle> find(vtcontrol::ObjectRef const& ref) const;

    /// Labels an object; an empty label clears it. Ignored for an object that is not bound. A label that
    /// changed is told to every label observer (@see onLabelChanged()).
    /// @param ref The object.
    /// @param label The label.
    void setLabel(vtcontrol::ObjectRef const& ref, std::string label);

    /// Subscribes @p observer to every change of a label setLabel() makes, of any kind of object, in
    /// subscription order. A label dropped with its object (forget()) is not a change: the object is gone.
    /// @param observer Told of each change, on the loop thread.
    /// @return The subscription; the observer is told nothing once it is destroyed.
    [[nodiscard]] Subscription onLabelChanged(LabelObserver observer);

    /// @param ref The object.
    /// @return Its label, or an empty view for none. Valid until the next change of that label.
    [[nodiscard]] std::string_view labelOf(vtcontrol::ObjectRef const& ref) const;

    /// @return Every bound object and every label, for a live upgrade to carry across.
    [[nodiscard]] ObjectRegistrySnapshot snapshot() const;

    /// Replaces everything bound and labelled with what @p snapshot holds: the other half of snapshot(), in
    /// the image a live upgrade started. A label of an object the snapshot does not bind is dropped, as
    /// setLabel() drops it.
    /// @param snapshot What snapshot() returned.
    void restore(ObjectRegistrySnapshot const& snapshot);

  private:
    /// One kind of object, in both directions.
    struct Table
    {
        std::unordered_map<uint64_t, vtcontrol::Uuid> byId;
        std::unordered_map<vtcontrol::Uuid, uint64_t> byUuid;
    };

    void bindIn(Table& table, vtcontrol::ObjectKind kind, uint64_t id, vtcontrol::Uuid uuid);
    void forgetIn(Table& table, vtcontrol::ObjectKind kind, uint64_t id);
    [[nodiscard]] static std::optional<vtcontrol::Uuid> uuidIn(Table const& table, uint64_t id);
    [[nodiscard]] Table const& tableOf(vtcontrol::ObjectKind kind) const noexcept;

    std::shared_ptr<vtcontrol::UuidSource> _uuids;
    Table _workspaces;
    Table _tabs;
    Table _panes;
    std::map<vtcontrol::ObjectRef, std::string> _labels;
    std::shared_ptr<LabelObservers> _labelObservers; ///< @see onLabelChanged()
};

} // namespace vthost
