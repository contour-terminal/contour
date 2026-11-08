// SPDX-License-Identifier: Apache-2.0
#include <vthost/handover/HandoverCodec.hpp>

#include <vtbackend/state/StateFields.hpp>

#include <algorithm>
#include <cmath>
#include <new>
#include <optional>
#include <ranges>
#include <utility>

#include <vthost/SessionSettings.hpp>

// The handover image reuses the terminal state's tolerant, id-keyed field scheme (vtbackend/state/
// StateFields.hpp): the types it carries from other modules are listed here, as overloads of
// visitStateFields() found through the visitor's namespace, and the enumerations among them cross by the
// stable tables below -- appended to, never reordered.
namespace vtbackend::detail
{

// {{{ wire tables -- appended to, never reordered

template <>
struct StateEnumWire<vthost::SettingsOrigin>
{
    static constexpr auto Values = std::array {
        vthost::SettingsOrigin::Profile,
        vthost::SettingsOrigin::Client,
    };
};

template <>
struct StateEnumWire<vthost::handover::SessionDevice>
{
    static constexpr auto Values = std::array {
        vthost::handover::SessionDevice::ChildOnPty,
        vthost::handover::SessionDevice::PtyOnly,
    };
};

template <>
struct StateEnumWire<vthost::DeliveryKind>
{
    static constexpr auto Values = std::array {
        vthost::DeliveryKind::Pty,
        vthost::DeliveryKind::Screen,
    };
};

template <>
struct StateEnumWire<vthost::DeliveryProgress>
{
    static constexpr auto Values = std::array {
        vthost::DeliveryProgress::Whole,
        vthost::DeliveryProgress::InterruptedRest,
    };
};

template <>
struct StateEnumWire<vtbackend::ColorPreference>
{
    static constexpr auto Values = std::array {
        vtbackend::ColorPreference::Dark,
        vtbackend::ColorPreference::Light,
    };
};

template <>
struct StateEnumWire<vtworkspace::SplitState>
{
    static constexpr auto Values = std::array {
        vtworkspace::SplitState::None,
        vtworkspace::SplitState::Horizontal,
        vtworkspace::SplitState::Vertical,
    };
};

template <>
struct StateEnumWire<vtworkspace::ZoomState>
{
    static constexpr auto Values = std::array {
        vtworkspace::ZoomState::Unzoomed,
        vtworkspace::ZoomState::Zoomed,
    };
};

template <>
struct StateEnumWire<vtcontrol::ObjectKind>
{
    static constexpr auto Values = std::array {
        vtcontrol::ObjectKind::Workspace,
        vtcontrol::ObjectKind::Tab,
        vtcontrol::ObjectKind::Pane,
    };
};

// }}}

namespace
{
    /// The deepest pane tree a stream may describe: the recursion that rebuilds it is bounded by this, not by
    /// the stream.
    constexpr auto MaxPaneTreeDepth = size_t { 1024 };

    /// One node of a pane tree on the wire. The tree crosses flattened, pre-order, each node naming how many
    /// children follow it: a nested encoding would let a stream choose the depth of the decoder's recursion.
    struct PaneNodeWire
    {
        vtworkspace::PaneId id {};
        vtworkspace::SplitState split = vtworkspace::SplitState::None;
        double ratio = 0.5;
        vtworkspace::SessionId session {};
        uint32_t childCount = 0;

        void visitFields(this auto& self, auto& visitor)
        {
            visitor(1, self.id);
            visitor(2, self.split);
            visitor(3, self.ratio);
            visitor(4, self.session);
            visitor(5, self.childCount);
        }
    };

    void flatten(vtworkspace::PaneTreeState const& node, std::vector<PaneNodeWire>& out)
    {
        out.push_back(PaneNodeWire { .id = node.id,
                                     .split = node.split,
                                     .ratio = node.ratio,
                                     .session = node.session,
                                     .childCount = static_cast<uint32_t>(node.children.size()) });
        for (auto const& child: node.children)
            flatten(child, out);
    }

    /// Rebuilds the node at @p next and the nodes under it.
    /// @return The node, or nothing when the run is too short, or deeper than MaxPaneTreeDepth.
    std::optional<vtworkspace::PaneTreeState> rebuild(std::span<PaneNodeWire const> nodes,
                                                      size_t& next,
                                                      size_t depth)
    {
        if (next >= nodes.size() || depth > MaxPaneTreeDepth)
            return std::nullopt;
        auto const& wire = nodes[next++];
        auto node = vtworkspace::PaneTreeState {
            .id = wire.id, .split = wire.split, .ratio = wire.ratio, .session = wire.session, .children = {}
        };
        for (auto const _: std::views::iota(uint32_t { 0 }, wire.childCount))
        {
            (void) _;
            auto child = rebuild(nodes, next, depth + 1);
            if (!child)
                return std::nullopt;
            node.children.push_back(std::move(*child));
        }
        return node;
    }
} // namespace

// {{{ foreign types

template <typename Visitor, typename T>
    requires std::same_as<std::remove_const_t<T>, vtcontrol::Uuid>
static void visitStateFields(Visitor& visitor, T& uuid)
{
    visitor(1, uuid.bytes);
}

template <typename Visitor, typename T>
    requires std::same_as<std::remove_const_t<T>, vtcontrol::ObjectRef>
static void visitStateFields(Visitor& visitor, T& ref)
{
    visitor(1, ref.kind);
    visitor(2, ref.uuid);
}

template <typename Visitor, typename T>
    requires std::same_as<std::remove_const_t<T>, vthost::CarriedDelivery>
static void visitStateFields(Visitor& visitor, T& delivery)
{
    visitor(1, delivery.kind);
    visitor(2, delivery.bytes);
    visitor(3, delivery.progress);
}

template <typename Visitor, typename T>
    requires std::same_as<std::remove_const_t<T>, vthost::proto::WireFrozenMode>
static void visitStateFields(Visitor& visitor, T& mode)
{
    visitor(1, mode.mode);
    visitor(2, mode.frozenAs);
}

template <typename Visitor, typename T>
    requires std::same_as<std::remove_const_t<T>, vthost::proto::WireSessionSettings>
static void visitStateFields(Visitor& visitor, T& wire)
{
    visitor(1, wire.historyLineCount);
    visitor(2, wire.terminalId);
    visitor(3, wire.graphemeClustering);
    visitor(4, wire.allowReflowOnResize);
    visitor(5, wire.maxImageRegisterCount);
    visitor(6, wire.sixelScrolling);
    visitor(7, wire.wordDelimiters);
    visitor(8, wire.frozenModes);
}

template <typename Visitor, typename T>
    requires std::same_as<std::remove_const_t<T>, vtpty::CrossingHandle>
static void visitStateFields(Visitor& visitor, T& handle)
{
    visitor(1, handle.value);
}

template <typename Visitor, typename T>
    requires std::same_as<std::remove_const_t<T>, vtpty::InheritedConPty>
static void visitStateFields(Visitor& visitor, T& conpty)
{
    visitor(1, conpty.outputReader);
    visitor(2, conpty.inputWriter);
    visitor(3, conpty.signal);
    visitor(4, conpty.reference);
    visitor(5, conpty.consoleHost);
}

template <typename Visitor, typename T>
    requires std::same_as<std::remove_const_t<T>, vtpty::InheritedPty>
static void visitStateFields(Visitor& visitor, T& pty)
{
    visitor(1, pty.masterFd);
    visitor(2, pty.stdoutFastPipe);
    visitor(3, pty.pageSize);
    visitor(4, pty.pixels);
    visitor(5, pty.conpty);
}

template <typename Visitor, typename T>
    requires std::same_as<std::remove_const_t<T>, vtpty::AdoptedChild>
static void visitStateFields(Visitor& visitor, T& child)
{
    visitor(1, child.pid);
    visitor(2, child.executable);
    visitor(3, child.args);
    // Only the duration of the grace crosses; its clock is the new process's own.
    visitor(5, child.hangupGrace.duration);
    visitor(6, child.processHandle);
    if constexpr (std::is_same_v<Visitor, FieldDecoder>)
    {
        auto cwd = std::string {};
        visitor(4, cwd);
        child.cwd = std::move(cwd);
    }
    else
        visitor(4, child.cwd.string());
}

template <typename Visitor, typename T>
    requires std::same_as<std::remove_const_t<T>, vtworkspace::ModelIdState>
static void visitStateFields(Visitor& visitor, T& ids)
{
    visitor(1, ids.nextWorkspace);
    visitor(2, ids.nextTab);
    visitor(3, ids.nextPane);
}

template <typename Visitor, typename T>
    requires std::same_as<std::remove_const_t<T>, vtworkspace::PaneTreeState>
static void visitStateFields(Visitor& visitor, T& root)
{
    if constexpr (std::is_same_v<Visitor, FieldDecoder>)
    {
        auto nodes = std::vector<PaneNodeWire> {};
        visitor(1, nodes);
        if (nodes.empty())
            return;
        auto next = size_t { 0 };
        auto tree = rebuild(nodes, next, 0);
        if (!tree || next != nodes.size())
        {
            visitor.fail(StateDecodeError::MalformedRecord);
            return;
        }
        root = std::move(*tree);
    }
    else
    {
        auto nodes = std::vector<PaneNodeWire> {};
        flatten(root, nodes);
        visitor(1, nodes);
    }
}

template <typename Visitor, typename T>
    requires std::same_as<std::remove_const_t<T>, vtworkspace::TabViewState>
static void visitStateFields(Visitor& visitor, T& view)
{
    visitor(1, view.activePane);
    visitor(2, view.zoom);
    visitor(3, view.recentPanes);
    visitor(4, view.title);
    visitor(5, view.colors);
}

template <typename Visitor, typename T>
    requires std::same_as<std::remove_const_t<T>, vtworkspace::TabState>
static void visitStateFields(Visitor& visitor, T& tab)
{
    visitor(1, tab.id);
    visitor(2, tab.root);
    visitor(3, tab.view);
}

template <typename Visitor, typename T>
    requires std::same_as<std::remove_const_t<T>, vtworkspace::WorkspaceState>
static void visitStateFields(Visitor& visitor, T& workspace)
{
    visitor(1, workspace.id);
    visitor(2, workspace.tabs);
    visitor(3, workspace.activeTabIndex);
    visitor(4, workspace.previousActiveTabIndex);
}

template <typename Visitor, typename T>
    requires std::same_as<std::remove_const_t<T>, vtworkspace::WorkspaceTreeState>
static void visitStateFields(Visitor& visitor, T& tree)
{
    visitor(1, tree.workspaces);
}

// }}}

} // namespace vtbackend::detail

namespace vthost::handover
{

namespace
{
    HandoverError toHandoverError(vtbackend::StateDecodeError error) noexcept
    {
        switch (error)
        {
            case vtbackend::StateDecodeError::Truncated: return HandoverError::Truncated;
            case vtbackend::StateDecodeError::UnknownFormatVersion:
                return HandoverError::UnknownFormatVersion;
            case vtbackend::StateDecodeError::MalformedRecord:
            case vtbackend::StateDecodeError::InvalidValue:
            case vtbackend::StateDecodeError::TooLarge: return HandoverError::Malformed;
        }
        return HandoverError::Malformed;
    }

    /// Gives every session whose PTY the image names no page size for -- a corrupt image; none is ever
    /// captured so -- FallbackPageSize, and refuses its terminal state: the program is adopted all the same,
    /// on a fresh terminal of that size, and no size anyone divides by is empty.
    void refuseEmptyPageSizes(HandoverImage& image)
    {
        for (auto& session: image.sessions)
        {
            if (session.pty.pageSize.lines.value > 0 && session.pty.pageSize.columns.value > 0)
                continue;
            session.pty.pageSize = FallbackPageSize;
            session.terminalState.clear();
        }
    }
} // namespace

std::vector<uint8_t> encodeHandover(HandoverImage const& image)
{
    auto writer = vtbackend::StateWriter::payload();
    writer.raw(HandoverMagic);
    writer.varint(HandoverFormatVersion);
    auto encoder = vtbackend::detail::FieldEncoder { writer };
    encoder.writeValue(image);
    return std::move(writer).take();
}

std::expected<HandoverImage, HandoverError> decodeHandover(std::span<uint8_t const> stream)
{
    auto reader = vtbackend::StateReader::ofPayload(stream);
    auto const magic = reader.raw(HandoverMagic.size());
    if (!magic)
        return std::unexpected { toHandoverError(magic.error()) };
    if (!std::ranges::equal(*magic, HandoverMagic))
        return std::unexpected { HandoverError::Malformed };
    auto const version = reader.varint();
    if (!version)
        return std::unexpected { toHandoverError(version.error()) };
    if (*version < OldestReadableHandoverFormat || *version > HandoverFormatVersion)
        return std::unexpected { HandoverError::UnknownFormatVersion };
    auto const payload = reader.bytes();
    if (!payload)
        return std::unexpected { toHandoverError(payload.error()) };
    if (!reader.atEnd())
        return std::unexpected { HandoverError::Malformed };
    try
    {
        auto image = HandoverImage {};
        if (auto const decoded = vtbackend::detail::decodeFields(*payload, image); !decoded)
            return std::unexpected { toHandoverError(decoded.error()) };
        refuseEmptyPageSizes(image);
        return image;
    }
    catch (std::bad_alloc const&)
    {
        // What the decode budget does not bound -- one string as long as the stream -- may still not fit.
        return std::unexpected { HandoverError::Malformed };
    }
}

} // namespace vthost::handover
