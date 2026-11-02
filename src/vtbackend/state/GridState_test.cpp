// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/grid/Grid.hpp>
#include <vtbackend/state/GridState.hpp>
#include <vtbackend/state/StateFields.hpp>
#include <vtbackend/testing/MockTerm.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <format>
#include <map>
#include <ranges>
#include <string>
#include <vector>

using namespace vtbackend;
using namespace std::string_view_literals;

namespace
{
/// Names fragments by a fixed table, standing in for the image table of a full capture.
class TableNamer final: public ImageFragmentNamer
{
  public:
    [[nodiscard]] uint64_t fragmentId(std::shared_ptr<ImageFragment> const& fragment) override
    {
        auto const [it, inserted] = _ids.try_emplace(fragment.get(), fragments.size());
        if (inserted)
            fragments.push_back(fragment);
        return it->second;
    }

    std::vector<std::shared_ptr<ImageFragment>> fragments; ///< By id.

  private:
    std::map<ImageFragment const*, uint64_t> _ids;
};

/// Resolves fragment ids against what a TableNamer filed.
class TableResolver final: public ImageFragmentResolver
{
  public:
    explicit TableResolver(std::vector<std::shared_ptr<ImageFragment>> fragments):
        _fragments { std::move(fragments) }
    {
    }

    [[nodiscard]] std::shared_ptr<ImageFragment> fragment(uint64_t id) const override
    {
        return id < _fragments.size() ? _fragments[id] : nullptr;
    }

    [[nodiscard]] std::shared_ptr<Image const> image(ImageId /*id*/) const override { return nullptr; }

  private:
    std::vector<std::shared_ptr<ImageFragment>> _fragments;
};

/// @return @p grid captured, with no image to name.
GridState captured(Grid const& grid)
{
    auto namer = TableNamer {};
    auto state = grid.captureState(namer);
    REQUIRE(namer.fragments.empty());
    return state;
}

/// @return @p state through the byte codec.
GridState throughBytes(GridState const& state)
{
    auto writer = StateWriter::payload();
    detail::encodeFields(writer, state);
    auto const bytes = std::move(writer).take();
    auto decoded = GridState {};
    REQUIRE(detail::decodeFields(bytes, decoded).has_value());
    return decoded;
}

/// A grid built as @p source was, restored from it through the byte codec.
Grid roundTripped(Grid const& source)
{
    auto restored = Grid { source.pageSize(), false, source.historyLimits() };
    REQUIRE(restored.restoreState(throughBytes(captured(source)), NoImagesResolver {}).has_value());
    return restored;
}

/// Every offset the grid holds, history first.
auto allOffsets(Grid const& grid)
{
    return std::views::iota(-unbox<int>(grid.historyLineCount()), unbox<int>(grid.pageSize().lines))
           | std::views::transform([](int offset) { return LineOffset(offset); });
}

void requireSameGrid(Grid const& expected, Grid const& actual)
{
    REQUIRE(actual.pageSize() == expected.pageSize());
    REQUIRE(actual.historyLineCount() == expected.historyLineCount());
    REQUIRE(actual.generation() == expected.generation());
    REQUIRE(actual.evictedRowCount() == expected.evictedRowCount());
    for (auto const offset: allOffsets(expected))
    {
        INFO(std::format("offset {}", unbox<int>(offset)));
        CHECK(actual.lineText(offset) == expected.lineText(offset));
        CHECK(actual.stableLineIdOf(offset) == expected.stableLineIdOf(offset));
        CHECK(actual.lineAt(offset).storage().codepoints == expected.lineAt(offset).storage().codepoints);
        CHECK(actual.lineAt(offset).storage().sgr == expected.lineAt(offset).storage().sgr);
    }
    CHECK(captured(actual) == captured(expected));
}

/// A stream of one grid, each of its lines in its own frame, with @p tamper applied to its line bytes.
template <typename Tamper>
std::expected<void, StateDecodeError> restoreTampered(Grid& grid, Tamper&& tamper)
{
    auto state = captured(grid);
    std::forward<Tamper>(tamper)(state);
    return grid.restoreState(state, NoImagesResolver {});
}
} // namespace

TEST_CASE("blank lines stay uninflated across a round trip", "[GridState]")
{
    auto mock = MockTerm { PageSize { LineCount(6), ColumnCount(10) }, LineCount(4) };
    mock.writeToScreen("x\r\n\r\n\r\nz"sv);
    auto const& grid = mock.terminal.primaryScreen().grid();
    REQUIRE(isBlankLineSoA(grid.lineAt(LineOffset(4)).storage()));

    auto const restored = roundTripped(grid);

    for (auto const offset: allOffsets(grid))
        CHECK(isBlankLineSoA(restored.lineAt(offset).storage())
              == isBlankLineSoA(grid.lineAt(offset).storage()));
    CHECK(isBlankLineSoA(restored.lineAt(LineOffset(4)).storage()));
    requireSameGrid(grid, restored);
}

TEST_CASE("grapheme clusters and wide cells survive", "[GridState]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(12) }, LineCount(2) };
    // A decomposed e with its combining accent, a CJK pair, and a coloured cell.
    mock.writeToScreen("e\xCC\x81 \xE6\xBC\xA2\xE5\xAD\x97 \033[1;31mR\033[m"sv);
    auto const& grid = mock.terminal.primaryScreen().grid();
    auto const& original = grid.lineAt(LineOffset(0)).storage();
    REQUIRE(std::ranges::any_of(original.clusterSize, [](uint8_t size) { return size >= 2; }));
    REQUIRE(std::ranges::any_of(original.widths, [](uint8_t width) { return width == 2; }));
    REQUIRE_FALSE(original.clusterPool.empty());

    auto const restored = roundTripped(grid);

    auto const& copy = restored.lineAt(LineOffset(0)).storage();
    CHECK(copy.codepoints == original.codepoints);
    CHECK(copy.widths == original.widths);
    CHECK(copy.clusterSize == original.clusterSize);
    CHECK(copy.clusterPoolIndex == original.clusterPoolIndex);
    CHECK(copy.clusterPool == original.clusterPool);
    CHECK(copy.sgr == original.sgr);
    CHECK(copy.shape == original.shape);
    CHECK(restored.lineText(LineOffset(0)) == grid.lineText(LineOffset(0)));
}

TEST_CASE("a line of plain text costs about a byte per cell", "[GridState]")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(200) }, LineCount(0) };
    mock.writeToScreen(std::string(200, 'x'));
    mock.writeToScreen("\r\n\033[31m"sv);
    mock.writeToScreen(std::string(100, 'y'));
    mock.writeToScreen("\033[1;44m"sv);
    mock.writeToScreen(std::string(100, 'z'));
    auto const state = captured(mock.terminal.primaryScreen().grid());
    // 400 cells: their text, a frame per line, and a handful of rendition runs.
    CHECK(state.lines.size() < 400 + 64);
    CHECK(state.renditions.size() == 3);
}

TEST_CASE("a full history ring with wrap-around restores line order and stable ids", "[GridState]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(8) }, LineCount(5) };
    for (auto const row: std::views::iota(0, 40))
        mock.writeToScreen(std::format("row{:02}\r\n", row));
    auto const& grid = mock.terminal.primaryScreen().grid();
    REQUIRE(grid.historyLineCount() == LineCount(5));
    REQUIRE(grid.stableLineIdOf(LineOffset(0)) > 10);

    auto const restored = roundTripped(grid);

    requireSameGrid(grid, restored);
    CHECK(restored.lineText(LineOffset(-1)) == grid.lineText(LineOffset(-1)));
    CHECK(restored.lineText(LineOffset(-5)) == grid.lineText(LineOffset(-5)));
}

TEST_CASE("trimmed history (evictedRowCount > 0) keeps stable row ids", "[GridState]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(8) }, LineCount(4) };
    for (auto const row: std::views::iota(0, 25))
        mock.writeToScreen(std::format("r{:02}\r\n", row));
    auto const& grid = mock.terminal.primaryScreen().grid();
    REQUIRE(grid.evictedRowCount() > 0);

    auto const restored = roundTripped(grid);

    CHECK(restored.evictedRowCount() == grid.evictedRowCount());
    CHECK(captured(restored).stableBase == captured(grid).stableBase);
    CHECK(captured(restored).stableFloor == captured(grid).stableFloor);
    requireSameGrid(grid, restored);
}

TEST_CASE("a restored grid keeps scrolling like the original", "[GridState]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(8) }, LineCount(5) };
    for (auto const row: std::views::iota(0, 12))
        mock.writeToScreen(std::format("a{:02}\r\n", row));
    auto& original = mock.terminal.primaryScreen().grid();
    auto copy = roundTripped(original);

    for ([[maybe_unused]] auto const step: std::views::iota(0, 20))
    {
        (void) original.scrollUp(LineCount(1));
        (void) copy.scrollUp(LineCount(1));
    }

    CHECK(captured(copy) == captured(original));
    CHECK(copy.evictedRowCount() == original.evictedRowCount());
    CHECK(copy.stableLineIdOf(LineOffset(0)) == original.stableLineIdOf(LineOffset(0)));
}

TEST_CASE("image fragments restore through the resolver, one object each", "[GridState]")
{
    auto const image = std::make_shared<Image>(
        ImageId(1), ImageFormat::RGBA, Image::Data(16, 0), ImageSize { Width(2), Height(2) }, [](auto) {});
    auto const rasterized =
        std::make_shared<RasterizedImage>(image,
                                          ImageAlignment::TopStart,
                                          ImageResize::NoResize,
                                          RGBAColor { 0, 0, 0, 0xFF },
                                          GridSize { .lines = LineCount(1), .columns = ColumnCount(2) },
                                          ImageSize { Width(1), Height(2) });
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(4) }, LineCount(1) };
    auto& grid = mock.terminal.primaryScreen().grid();
    auto& fragments = grid.lineAt(LineOffset(0)).storage().imageFragments.emplace();
    fragments.emplace(uint16_t { 0 }, std::make_shared<ImageFragment>(rasterized, CellLocation {}));
    fragments.emplace(
        uint16_t { 1 },
        std::make_shared<ImageFragment>(rasterized, CellLocation { LineOffset(0), ColumnOffset(1) }));

    auto namer = TableNamer {};
    auto const state = throughBytes(grid.captureState(namer));
    REQUIRE(namer.fragments.size() == 2);

    SECTION("a resolver that knows the ids hands every cell its own object back")
    {
        auto restored = Grid { grid.pageSize(), false, grid.historyLimits() };
        REQUIRE(restored.restoreState(state, TableResolver { namer.fragments }).has_value());
        auto const& map = restored.lineAt(LineOffset(0)).storage().imageFragments;
        REQUIRE(map.has_value());
        REQUIRE(map->size() == 2);
        CHECK(map->at(0).get() == fragments.at(0).get());
        CHECK(map->at(1).get() == fragments.at(1).get());
    }

    SECTION("a resolver that refuses every reference rejects the grid")
    {
        auto restored = Grid { grid.pageSize(), false, grid.historyLimits() };
        CHECK(restored.restoreState(state, NoImagesResolver {})
              == std::unexpected { StateDecodeError::InvalidValue });
    }
}

TEST_CASE("restoring an inconsistent state is refused and leaves the grid untouched", "[GridState]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(8) }, LineCount(5) };
    mock.writeToScreen("keep\r\nme"sv);
    auto& grid = mock.terminal.primaryScreen().grid();
    auto const before = captured(grid);

    SECTION("a line count that disagrees with linesUsed")
    {
        CHECK(
            restoreTampered(grid, [](GridState& state) { state.linesUsed = state.linesUsed + LineCount(1); })
            == std::unexpected { StateDecodeError::InvalidValue });
    }
    SECTION("a cluster that points outside the pool")
    {
        auto& line = grid.lineAt(LineOffset(0));
        line.storage().clusterSize[0] = 3;
        CHECK(restoreTampered(grid, [](GridState&) {}) == std::unexpected { StateDecodeError::InvalidValue });
        line.storage().clusterSize[0] = 1;
    }
    SECTION("a rendition index outside the table")
    {
        CHECK(restoreTampered(grid, [](GridState& state) { state.renditions.clear(); })
              == std::unexpected { StateDecodeError::InvalidValue });
    }
    SECTION("history deeper than the capacity")
    {
        CHECK(
            restoreTampered(grid, [](GridState& state) { state.limits = HistoryLimits::plain(LineCount(0)); })
            == std::unexpected { StateDecodeError::InvalidValue });
    }

    CHECK(captured(grid) == before);
}

TEST_CASE("a truncated grid stream is Truncated", "[GridState]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(8) }, LineCount(5) };
    mock.writeToScreen("abc"sv);
    auto state = captured(mock.terminal.primaryScreen().grid());
    state.lines.resize(state.lines.size() - 2);
    auto& grid = mock.terminal.primaryScreen().grid();
    auto const result = grid.restoreState(state, NoImagesResolver {});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == StateDecodeError::Truncated);
}
