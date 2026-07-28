// SPDX-License-Identifier: Apache-2.0
#include <vthost/tmux/LayoutString.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <format>

#include <vtworkspace/PaneLayout.h>

namespace vthost::tmux
{

using vtworkspace::SplitState;

std::uint16_t layoutChecksum(std::string_view body) noexcept
{
    // tmux layout-custom.c layout_checksum: rotate right by one, then add.
    auto csum = std::uint16_t { 0 };
    for (auto const ch: body)
    {
        csum = static_cast<std::uint16_t>((csum >> 1U) + ((csum & 1U) << 15U));
        csum = static_cast<std::uint16_t>(csum + static_cast<unsigned char>(ch));
    }
    return csum;
}

namespace
{
    void encodeNode(std::string& out, vtworkspace::Pane const& node, int x, int y, int width, int height)
    {
        out += std::format("{}x{},{},{}", width, height, x, y);
        if (node.isLeaf())
        {
            out += std::format(",{}", node.id().value);
            return;
        }

        auto const sideBySide = node.splitState() == SplitState::Vertical;
        auto const axisExtent = sideBySide ? width : height;
        // Split against each subtree's own minimum, not a flat one cell: `layout_check` demands
        // `first + divider + second == parent`, which only survives if neither child is squeezed
        // below what its own splits need. encodeLayout() has already sized the area so both fit.
        auto const alongAxis = [sideBySide](vtworkspace::LayoutSize extent) {
            return sideBySide ? extent.width : extent.height;
        };
        auto const [first, second] =
            vtworkspace::splitCellExtents(axisExtent,
                                          node.ratio(),
                                          alongAxis(vtworkspace::minimumCellArea(*node.first())),
                                          alongAxis(vtworkspace::minimumCellArea(*node.second())));

        out += sideBySide ? '{' : '[';
        if (sideBySide)
        {
            encodeNode(out, *node.first(), x, y, first, height);
            out += ',';
            encodeNode(out, *node.second(), x + first + 1, y, second, height);
        }
        else
        {
            encodeNode(out, *node.first(), x, y, width, first);
            out += ',';
            encodeNode(out, *node.second(), x, y + first + 1, width, second);
        }
        out += sideBySide ? '}' : ']';
    }
} // namespace

std::string encodeLayout(vtworkspace::Pane const& root, vtpty::PageSize area)
{
    // Solve into at least the tree's own minimum. An area too small for the tree has NO valid layout
    // string — `first + divider + second == parent` cannot hold once a split axis drops below three
    // cells — and a checksum-valid but geometrically INVALID layout is the worst of the three
    // outcomes: every consumer rejects it (our own parseLayout says "children do not partition the
    // parent", TmuxClientModel::ingestLayout then leaves the previous state standing), so the
    // attached client's pane tree silently stops tracking the daemon. Describing a window slightly
    // larger than the client area is what tmux itself does, and clients simply resize into it.
    auto const minimum = vtworkspace::minimumCellArea(root);
    auto const width = std::max(unbox<int>(area.columns), minimum.width);
    auto const height = std::max(unbox<int>(area.lines), minimum.height);

    auto body = std::string {};
    encodeNode(body, root, 0, 0, width, height);
    return std::format("{:04x},{}", layoutChecksum(body), body);
}

namespace
{
    /// The deepest container nesting parseLayout accepts. A layout string arrives
    /// over the (impersonable) control channel behind only a forgeable checksum, so
    /// the recursive-descent parser — and the check()/collapse passes that recurse
    /// over its output — must be bounded, or a maliciously deep `{`/`[` spine
    /// overflows the call stack. Real pane trees nest a handful deep; this is far
    /// above any legitimate layout yet well within the stack budget.
    constexpr std::size_t MaxNestingDepth = 256;

    /// The most nodes one layout string may describe.
    ///
    /// MaxNestingDepth bounds the DEPTH axis; this bounds the BREADTH one, which was left open.
    /// A single flat container may list as many children as its extents allow, and the collapse
    /// pass turns a container's tail into a right-leaning CHAIN — so its (recursive) destructor,
    /// `leafCount()`, and every consumer walk over the result are one frame per sibling. A ~1 MiB
    /// `%layout-change` naming ~131,000 one-cell leaves fits the reader's line limit and drove the
    /// process into a stack overflow. Real windows hold a handful of panes, and a pane costs at
    /// least a column plus a divider, so this stays far above any layout a terminal can display.
    constexpr std::size_t MaxNodeCount = 1024;

    /// Recursive-descent state over the layout body.
    struct Parser
    {
        std::string_view text;
        std::size_t pos = 0;
        std::size_t nodes = 0; ///< Nodes built so far, against MaxNodeCount.

        [[nodiscard]] bool atEnd() const noexcept { return pos >= text.size(); }
        [[nodiscard]] char peek() const noexcept { return pos < text.size() ? text[pos] : '\0'; }

        [[nodiscard]] bool consume(char expected) noexcept
        {
            if (peek() != expected)
                return false;
            ++pos;
            return true;
        }

        [[nodiscard]] std::optional<int> number() noexcept
        {
            auto value = 0;
            auto const [ptr, ec] = std::from_chars(text.data() + pos, text.data() + text.size(), value);
            if (ec != std::errc {} || ptr == text.data() + pos)
                return std::nullopt;
            pos = static_cast<std::size_t>(ptr - text.data());
            return value;
        }

        [[nodiscard]] std::expected<ParsedLayout, std::string> cell(std::size_t depth = 0)
        {
            if (depth > MaxNestingDepth)
                return std::unexpected("layout nesting too deep");
            if (++nodes > MaxNodeCount)
                return std::unexpected("layout has too many nodes");

            auto node = ParsedLayout {};

            auto const width = number();
            if (!width || !consume('x'))
                return std::unexpected("expected WxH");
            auto const height = number();
            if (!height || !consume(','))
                return std::unexpected("expected height and x offset");
            auto const x = number();
            if (!x || !consume(','))
                return std::unexpected("expected x and y offset");
            auto const y = number();
            if (!y)
                return std::unexpected("expected y offset");

            node.width = *width;
            node.height = *height;
            node.x = *x;
            node.y = *y;

            switch (peek())
            {
                case ',': {
                    // tmux's backtracking quirk (layout-custom.c:351-358): the digits
                    // after this comma are a pane id ONLY if not followed by 'x' —
                    // otherwise they are the next sibling's width and belong to the
                    // parent's list, so rewind as if this comma were never consumed.
                    auto const saved = pos;
                    ++pos; // consume ','
                    auto const id = number();
                    if (!id)
                        return std::unexpected("expected pane id");
                    if (peek() == 'x')
                    {
                        pos = saved;
                        node.kind = ParsedLayout::Kind::Leaf;
                        return node;
                    }
                    node.kind = ParsedLayout::Kind::Leaf;
                    node.paneId = static_cast<std::uint64_t>(*id);
                    return node;
                }
                case '{':
                case '[': {
                    auto const open = peek();
                    auto const close = open == '{' ? '}' : ']';
                    node.kind = open == '{' ? ParsedLayout::Kind::SideBySide : ParsedLayout::Kind::Stacked;
                    ++pos;
                    while (true)
                    {
                        auto child = cell(depth + 1);
                        if (!child)
                            return child;
                        node.children.push_back(std::move(*child));
                        if (consume(','))
                            continue;
                        break;
                    }
                    if (!consume(close))
                        return std::unexpected(std::format("expected '{}'", close));
                    if (node.children.size() < 2)
                        return std::unexpected("container needs at least two children");
                    return node;
                }
                default:
                    // A bare leaf without a pane id (tmux emits ids, but the grammar
                    // tolerates their absence in nested positions).
                    node.kind = ParsedLayout::Kind::Leaf;
                    return node;
            }
        }
    };

    /// Mirrors tmux's layout_check: children partition the parent exactly along
    /// the split axis (one divider cell between neighbours) and match it on the
    /// cross axis.
    [[nodiscard]] std::expected<void, std::string> check(ParsedLayout const& node)
    {
        if (node.kind == ParsedLayout::Kind::Leaf)
            return {};

        auto const sideBySide = node.kind == ParsedLayout::Kind::SideBySide;
        // Accumulate in 64 bits and widen each extent BEFORE the `+ 1`: the extents
        // come from from_chars into int and can be up to INT_MAX on a hostile
        // layout, so summing them in an int would overflow (signed-overflow UB —
        // and a UBSan abort on dev/CI builds). node.width/height fit in int, so the
        // widened comparison is exact.
        auto axisSum = std::int64_t { 0 };
        for (auto const& child: node.children)
        {
            if (sideBySide ? child.height != node.height : child.width != node.width)
                return std::unexpected("child cross-axis extent differs from parent");
            axisSum += static_cast<std::int64_t>(sideBySide ? child.width : child.height) + 1;
            if (auto nested = check(child); !nested)
                return nested;
        }
        if (axisSum - 1 != static_cast<std::int64_t>(sideBySide ? node.width : node.height))
            return std::unexpected("children do not partition the parent");
        return {};
    }
} // namespace

std::expected<ParsedLayout, std::string> parseLayout(std::string_view text)
{
    auto const comma = text.find(',');
    if (comma != 4 || text.size() < 5)
        return std::unexpected("expected 4-digit checksum prefix");

    auto checksumDigits = std::array<char, 4> {};
    std::ranges::copy(text.substr(0, 4), checksumDigits.begin());
    auto expectedChecksum = std::uint16_t {};
    auto const [ptr, ec] = std::from_chars(
        checksumDigits.data(), checksumDigits.data() + checksumDigits.size(), expectedChecksum, 16);
    if (ec != std::errc {} || ptr != checksumDigits.data() + checksumDigits.size())
        return std::unexpected("invalid checksum");

    auto const body = text.substr(comma + 1);
    if (layoutChecksum(body) != expectedChecksum)
        return std::unexpected("checksum mismatch");

    auto parser = Parser { .text = body };
    auto root = parser.cell();
    if (!root)
        return root;
    if (!parser.atEnd())
        return std::unexpected("trailing garbage after layout");
    if (auto valid = check(*root); !valid)
        return std::unexpected(valid.error());
    return root;
}

int BinaryLayout::leafCount() const noexcept
{
    if (!first)
        return 1;
    return first->leafCount() + second->leafCount();
}

namespace
{
    /// Collapses @p children (a container's tail) into a right-leaning chain.
    ///
    /// Built from the TAIL backwards, one loop step per sibling. Recursing per sibling instead —
    /// which is what this did — was bounded by nothing: MaxNestingDepth caps how deeply containers
    /// NEST, while a single flat container lists as many children as it likes, so a wide
    /// `{a,b,c,…}` off the (impersonable) control channel drove one stack frame per leaf. The
    /// parser's MaxNodeCount now bounds the breadth too, but the shape a hostile input has to
    /// exploit should not be a recursion in the first place.
    [[nodiscard]] BinaryLayout collapseChain(ParsedLayout const& parent, std::size_t from)
    {
        auto const sideBySide = parent.kind == ParsedLayout::Kind::SideBySide;
        auto const extentOf = [sideBySide](ParsedLayout const& node) {
            return static_cast<std::int64_t>(sideBySide ? node.width : node.height);
        };

        auto const last = parent.children.size() - 1;
        auto node = collapseToBinary(parent.children[last]);
        // The extent the chain node being built covers along the split axis, accumulated from the
        // tail: `sum(extent + 1) - 1` over the siblings it spans. In 64 bits because the extents
        // come from from_chars into int and reach INT_MAX on a hostile layout — collapseToBinary
        // is public, so it is not always preceded by check().
        auto remaining = extentOf(parent.children[last]);
        for (auto i = last; i-- > from;)
        {
            auto const& head = parent.children[i];
            auto const headExtent = extentOf(head);
            remaining += headExtent + 1;

            auto chain = BinaryLayout {};
            chain.orientation = sideBySide ? SplitState::Vertical : SplitState::Horizontal;
            // The head's share of this chain node, inverse of splitCellExtents:
            // first = round(ratio * (extent - 1)).
            chain.ratio =
                remaining > 1 ? static_cast<double>(headExtent) / static_cast<double>(remaining - 1) : 0.5;
            chain.first = std::make_unique<BinaryLayout>(collapseToBinary(head));
            chain.second = std::make_unique<BinaryLayout>(std::move(node));
            node = std::move(chain);
        }
        return node;
    }
} // namespace

BinaryLayout collapseToBinary(ParsedLayout const& layout)
{
    // A childless container is not something parseLayout can produce (it rejects a container with
    // fewer than two children), but this entry point is public: treat it as the leaf it
    // structurally is rather than indexing an empty vector.
    if (layout.kind == ParsedLayout::Kind::Leaf || layout.children.empty())
    {
        auto leaf = BinaryLayout {};
        leaf.paneId = layout.paneId;
        return leaf;
    }
    return collapseChain(layout, 0);
}

} // namespace vthost::tmux
