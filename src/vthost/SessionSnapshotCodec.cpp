// SPDX-License-Identifier: Apache-2.0
#include <vthost/SessionSnapshotCodec.hpp>

#include <core/Utf8Path.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <variant>

#include <vthost/proto/Wire.hpp>

namespace vthost
{

namespace
{
    constexpr auto Magic = std::array<std::byte, 8> {
        std::byte { 'C' }, std::byte { 'T' }, std::byte { 'R' }, std::byte { 'S' },
        std::byte { 'N' }, std::byte { 'A' }, std::byte { 'P' }, std::byte { '\n' },
    };

    constexpr auto RatioScale = 1'000'000.0;
    constexpr auto RatioScaleInt = std::uint64_t { 1'000'000 };

    // --- writing -----------------------------------------------------------------------------------

    template <typename Fill>
    void writeBlob(proto::Writer& sink, Fill&& fill)
    {
        auto inner = proto::Writer {};
        std::invoke(std::forward<Fill>(fill), inner);
        sink.blob(inner.view());
    }

    template <typename T, typename Put>
    void writeOptional(proto::Writer& sink, std::optional<T> const& value, Put&& put)
    {
        sink.u8(value.has_value() ? 1 : 0);
        if (value)
            std::invoke(std::forward<Put>(put), sink, *value);
    }

    void writeString(proto::Writer& sink, std::string const& text)
    {
        sink.string(text);
    }

    void writeIdentity(proto::Writer& sink, std::optional<ObjectIdentityRecord> const& identity)
    {
        writeOptional(sink, identity, [](proto::Writer& out, ObjectIdentityRecord const& record) {
            out.bytes(std::as_bytes(std::span { record.uuid }));
            writeOptional(out, record.label, writeString);
        });
    }

    void writeLeaf(proto::Writer& sink, PaneSnapshot const& pane)
    {
        writeIdentity(sink, pane.identity);
        writeOptional(sink, pane.workingDirectory, [](proto::Writer& out, std::filesystem::path const& path) {
            out.string(core::pathToUtf8(path));
        });
        writeOptional(sink, pane.profile, writeString);
        writeOptional(sink, pane.foregroundProcess, writeString);
        writeOptional(sink, pane.runningCommandLine, writeString);
        writeOptional(sink, pane.history, [](proto::Writer& out, proto::Delta const& delta) {
            auto frame = proto::Writer {};
            proto::encodePdu(frame, 0, delta);
            out.blob(frame.view());
        });
    }

    void writeNode(proto::Writer& sink, PaneNode const& node)
    {
        writeBlob(sink, [&](proto::Writer& out) {
            out.u8(static_cast<std::uint8_t>(node.split));
            if (node.split == vtworkspace::SplitState::None)
            {
                writeLeaf(out, node.pane);
                return;
            }
            out.varint(static_cast<std::uint64_t>(std::lround(node.ratio * RatioScale)));
            for (auto const& child: node.children)
                writeNode(out, child);
        });
    }

    void writeTab(proto::Writer& sink, TabSnapshot const& tab)
    {
        writeBlob(sink, [&](proto::Writer& out) {
            writeIdentity(out, tab.identity);
            writeOptional(out, tab.title, writeString);
            writeOptional(out, tab.color, [](proto::Writer& put, vtbackend::RGBColor const& color) {
                put.u32(color.value());
            });
            out.varint(tab.activeLeaf);
            out.u8(static_cast<std::uint8_t>(tab.zoom));
            writeNode(out, tab.root);
        });
    }

    void writeWindow(proto::Writer& sink, WindowSnapshot const& window)
    {
        writeBlob(sink, [&](proto::Writer& out) {
            writeIdentity(out, window.identity);
            out.varint(window.activeTab);
            out.varint(window.tabs.size());
            for (auto const& tab: window.tabs)
                writeTab(out, tab);
        });
    }

    // --- reading -----------------------------------------------------------------------------------

    template <typename T>
    using Parsed = std::expected<T, SnapshotError>;

    /// A wire value, or Malformed when the bytes ran out or lied.
    template <typename T>
    [[nodiscard]] Parsed<T> checked(std::expected<T, proto::DecodeError> value)
    {
        if (!value)
            return std::unexpected(SnapshotError::Malformed);
        return *std::move(value);
    }

    template <typename T, typename Get>
    [[nodiscard]] Parsed<std::optional<T>> readOptional(proto::Reader& source, Get&& get)
    {
        auto const present = source.u8();
        if (!present || *present > 1)
            return std::unexpected(SnapshotError::Malformed);
        if (*present == 0)
            return std::optional<T> {};
        return std::invoke(std::forward<Get>(get), source).transform([](T value) {
            return std::optional<T> { std::move(value) };
        });
    }

    [[nodiscard]] Parsed<std::string> readString(proto::Reader& source)
    {
        return checked(source.string());
    }

    [[nodiscard]] Parsed<std::optional<ObjectIdentityRecord>> readIdentity(proto::Reader& source)
    {
        return readOptional<ObjectIdentityRecord>(
            source, [](proto::Reader& in) -> Parsed<ObjectIdentityRecord> {
                auto record = ObjectIdentityRecord {};
                auto const raw = in.bytes(record.uuid.size());
                if (!raw)
                    return std::unexpected(SnapshotError::Malformed);
                std::ranges::transform(
                    *raw, record.uuid.begin(), [](std::byte b) { return static_cast<std::uint8_t>(b); });
                auto label = readOptional<std::string>(in, readString);
                if (!label)
                    return std::unexpected(label.error());
                record.label = *std::move(label);
                return record;
            });
    }

    /// Decodes a history frame; nullopt when it is not a Delta frame that spans exactly the blob.
    [[nodiscard]] std::optional<proto::Delta> decodeHistory(std::span<std::byte const> frame)
    {
        auto decoded = proto::decodePdu(frame);
        if (!decoded || decoded->consumed != frame.size())
            return std::nullopt;
        auto* const delta = std::get_if<proto::Delta>(&decoded->pdu);
        if (delta == nullptr)
            return std::nullopt;
        return std::move(*delta);
    }

    /// What one decode accumulates across the whole body.
    struct DecodeContext
    {
        std::size_t droppedHistories = 0;
        std::size_t leavesInTab = 0;
    };

    /// Runs @p read over the next blob of @p source, in a reader of its own: what the blob holds past
    /// what @p read knows is skipped.
    template <typename Read>
    [[nodiscard]] auto readBlobWith(proto::Reader& source, Read&& read)
        -> decltype(std::invoke(std::forward<Read>(read), std::declval<proto::Reader&>()))
    {
        auto const blob = source.blob();
        if (!blob)
            return std::unexpected(SnapshotError::Malformed);
        auto inner = proto::Reader { *blob };
        return std::invoke(std::forward<Read>(read), inner);
    }

    [[nodiscard]] Parsed<PaneSnapshot> readLeaf(proto::Reader& in, DecodeContext& context)
    {
        auto pane = PaneSnapshot {};
        // Each step reads one field; a blob that ends before a step leaves it, and the ones after it, at
        // their defaults.
        if (in.remaining() == 0)
            return pane;
        if (auto identity = readIdentity(in); identity)
            pane.identity = *std::move(identity);
        else
            return std::unexpected(identity.error());

        auto const readTextField = [&](std::optional<std::string>& field) -> Parsed<bool> {
            if (in.remaining() == 0)
                return false;
            auto text = readOptional<std::string>(in, readString);
            if (!text)
                return std::unexpected(text.error());
            field = *std::move(text);
            return true;
        };

        if (in.remaining() != 0)
        {
            auto cwd = readOptional<std::string>(in, readString);
            if (!cwd)
                return std::unexpected(cwd.error());
            if (*cwd)
                if (auto path = core::pathFromUtf8(**cwd); path)
                    pane.workingDirectory = *std::move(path);
        }
        for (auto* const field: { &pane.profile, &pane.foregroundProcess, &pane.runningCommandLine })
        {
            auto const more = readTextField(*field);
            if (!more)
                return std::unexpected(more.error());
            if (!*more)
                return pane;
        }
        if (in.remaining() == 0)
            return pane;
        auto history = readOptional<std::span<std::byte const>>(
            in, [](proto::Reader& source) { return checked(source.blob()); });
        if (!history)
            return std::unexpected(history.error());
        if (*history)
        {
            pane.history = decodeHistory(**history);
            if (!pane.history)
                ++context.droppedHistories;
        }
        return pane;
    }

    [[nodiscard]] Parsed<PaneNode> readNode(proto::Reader& source, DecodeContext& context, std::size_t depth)
    {
        return readBlobWith(source, [&](proto::Reader& in) -> Parsed<PaneNode> {
            auto node = PaneNode {};
            auto const split = in.u8();
            if (!split || *split > static_cast<std::uint8_t>(vtworkspace::SplitState::Vertical))
                return std::unexpected(SnapshotError::Malformed);
            node.split = static_cast<vtworkspace::SplitState>(*split);
            if (node.split == vtworkspace::SplitState::None)
            {
                if (++context.leavesInTab > MaxLeavesPerTab)
                    return std::unexpected(SnapshotError::Malformed);
                return readLeaf(in, context).transform([&](PaneSnapshot pane) {
                    node.pane = std::move(pane);
                    return std::move(node);
                });
            }
            if (depth >= MaxPaneDepth)
                return std::unexpected(SnapshotError::Malformed);
            auto const ratio = in.varint();
            if (!ratio || *ratio == 0 || *ratio >= RatioScaleInt)
                return std::unexpected(SnapshotError::Malformed);
            node.ratio = static_cast<double>(*ratio) / RatioScale;
            for ([[maybe_unused]] auto const slot: std::views::iota(0, 2))
            {
                auto child = readNode(in, context, depth + 1);
                if (!child)
                    return std::unexpected(child.error());
                node.children.push_back(*std::move(child));
            }
            return node;
        });
    }

    [[nodiscard]] Parsed<TabSnapshot> readTab(proto::Reader& source, DecodeContext& context)
    {
        return readBlobWith(source, [&](proto::Reader& in) -> Parsed<TabSnapshot> {
            auto tab = TabSnapshot {};
            context.leavesInTab = 0;
            auto identity = readIdentity(in);
            if (!identity)
                return std::unexpected(identity.error());
            tab.identity = *std::move(identity);
            auto title = readOptional<std::string>(in, readString);
            if (!title)
                return std::unexpected(title.error());
            tab.title = *std::move(title);
            auto color = readOptional<std::uint32_t>(in, [](proto::Reader& r) { return checked(r.u32()); });
            if (!color)
                return std::unexpected(color.error());
            if (*color)
                tab.color = vtbackend::RGBColor { **color };
            auto const activeLeaf = in.varint();
            auto const zoom = in.u8();
            if (!activeLeaf || !zoom || *activeLeaf > MaxLeavesPerTab
                || *zoom > static_cast<std::uint8_t>(TabZoom::Zoomed))
                return std::unexpected(SnapshotError::Malformed);
            tab.activeLeaf = static_cast<std::uint32_t>(*activeLeaf);
            tab.zoom = static_cast<TabZoom>(*zoom);
            auto root = readNode(in, context, 0);
            if (!root)
                return std::unexpected(root.error());
            tab.root = *std::move(root);
            return tab;
        });
    }

    [[nodiscard]] Parsed<WindowSnapshot> readWindow(proto::Reader& source, DecodeContext& context)
    {
        return readBlobWith(source, [&](proto::Reader& in) -> Parsed<WindowSnapshot> {
            auto window = WindowSnapshot {};
            if (in.remaining() == 0)
                return window;
            auto identity = readIdentity(in);
            if (!identity)
                return std::unexpected(identity.error());
            window.identity = *std::move(identity);
            if (in.remaining() == 0)
                return window;
            auto const activeTab = in.varint();
            auto const tabCount = in.varint();
            if (!activeTab || !tabCount || *tabCount > MaxTabsPerWindow)
                return std::unexpected(SnapshotError::Malformed);
            window.activeTab = static_cast<std::uint32_t>(std::min<std::uint64_t>(*activeTab, UINT32_MAX));
            for ([[maybe_unused]] auto const index: std::views::iota(std::uint64_t { 0 }, *tabCount))
            {
                auto tab = readTab(in, context);
                if (!tab)
                    return std::unexpected(tab.error());
                window.tabs.push_back(*std::move(tab));
            }
            return window;
        });
    }

    [[nodiscard]] Parsed<DecodedSnapshot> readBody(std::span<std::byte const> body)
    {
        auto in = proto::Reader { body };
        auto context = DecodeContext {};
        auto decoded = DecodedSnapshot {};
        auto const takenAt = in.svarint();
        auto const windowCount = in.varint();
        if (!takenAt || !windowCount || *windowCount > MaxSnapshotWindows)
            return std::unexpected(SnapshotError::Malformed);
        decoded.snapshot.takenAt = std::chrono::sys_seconds { std::chrono::seconds { *takenAt } };
        for ([[maybe_unused]] auto const index: std::views::iota(std::uint64_t { 0 }, *windowCount))
        {
            auto window = readWindow(in, context);
            if (!window)
                return std::unexpected(window.error());
            decoded.snapshot.windows.push_back(*std::move(window));
        }
        decoded.droppedHistories = context.droppedHistories;
        return decoded;
    }
} // namespace

std::vector<std::byte> encodeSnapshot(WorkspaceSnapshot const& snapshot)
{
    auto body = proto::Writer {};
    body.svarint(snapshot.takenAt.time_since_epoch().count());
    body.varint(snapshot.windows.size());
    for (auto const& window: snapshot.windows)
        writeWindow(body, window);

    auto file = proto::Writer {};
    file.bytes(Magic);
    file.varint(SnapshotFormatVersion);
    file.varint(body.size());
    auto const checksum = fnv1a64(body.view());
    file.u32(static_cast<std::uint32_t>(checksum));
    file.u32(static_cast<std::uint32_t>(checksum >> 32));
    file.bytes(body.view());
    return file.take();
}

std::expected<DecodedSnapshot, SnapshotError> decodeSnapshot(std::span<std::byte const> file)
{
    if (file.size() > MaxSnapshotFileBytes)
        return std::unexpected(SnapshotError::TooLarge);

    auto const seen = file.first(std::min(file.size(), Magic.size()));
    if (!std::ranges::equal(seen, std::span { Magic }.first(seen.size())))
        return std::unexpected(SnapshotError::BadMagic);

    auto in = proto::Reader { file.subspan(seen.size()) };
    if (seen.size() < Magic.size())
        return std::unexpected(SnapshotError::Truncated);
    auto const version = in.varint();
    if (!version)
        return std::unexpected(SnapshotError::Truncated);
    if (*version != SnapshotFormatVersion)
        return std::unexpected(SnapshotError::UnsupportedFormat);
    auto const bodyLength = in.varint();
    auto const low = in.u32();
    auto const high = in.u32();
    if (!bodyLength || !low || !high || in.remaining() < *bodyLength)
        return std::unexpected(SnapshotError::Truncated);
    if (in.remaining() > *bodyLength)
        return std::unexpected(SnapshotError::Malformed);

    auto const body = file.last(static_cast<std::size_t>(*bodyLength));
    if (fnv1a64(body) != (static_cast<std::uint64_t>(*high) << 32 | *low))
        return std::unexpected(SnapshotError::ChecksumMismatch);
    return readBody(body);
}

} // namespace vthost
