// SPDX-License-Identifier: Apache-2.0
//
// What the native relation answers while the tree it walks is renamed under it, on every OS: the
// identity of each parent counts, and a walk that a rename could have misled answers Busy (its
// chains walked again differ on POSIX, its final paths on Windows), never a wrong relation.
#include <core/fs/Descriptor.hpp>
#include <core/fs/DescriptorTestSupport.hpp>
#include <core/fs/FsError.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

using core::fs::DescriptorFlags;
using core::fs::FsErrorCode;
using core::fs::FsResult;
using core::fs::IDescriptor;
using core::fs::OpenFlags;
using core::fs::Relation;
using core::fs::SymlinkPolicy;
using core::fs::test::DescriptorFixture;

namespace
{

/// A thread that makes a cycle of renames in the host's file system over and over, until it is
/// destroyed, and counts the cycles in which every rename succeeded. Test-only, so it may own a
/// thread.
class Renamer
{
  public:
    /// One rename of a cycle.
    struct Step
    {
        std::filesystem::path from;
        std::filesystem::path to;
    };

    /// @param cycle The renames, made in this order; after the last, the tree is as it was.
    /// @param pause How long each rename's result stands before the next is made: zero renames as
    ///        fast as the system can, and a pause gives a slow walk a tree that holds still between.
    Renamer(std::vector<Step> cycle, std::chrono::microseconds pause): _state { std::make_unique<State>() }
    {
        _thread = std::thread { [state = _state.get(), cycle = std::move(cycle), pause] {
            while (!state->stop.load())
            {
                auto completed = true;
                for (auto const& step: cycle)
                {
                    auto error = std::error_code {};
                    std::filesystem::rename(step.from, step.to, error);
                    completed = completed && !error;
                    if (pause.count() > 0)
                        std::this_thread::sleep_for(pause);
                }
                if (completed)
                    state->exchanges.fetch_add(1);
            }
        } };
    }

    Renamer(Renamer const&) = delete;
    Renamer(Renamer&&) = delete;
    Renamer& operator=(Renamer const&) = delete;
    Renamer& operator=(Renamer&&) = delete;

    ~Renamer()
    {
        _state->stop.store(true);
        _thread.join();
    }

    /// Waits, bounded, for the first cycle in which every rename succeeded.
    /// @return Whether one did.
    [[nodiscard]] bool started() const
    {
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds { 10 };
        while (exchanges() == 0 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        return exchanges() > 0;
    }

    /// @return How many cycles have completed with every rename made.
    [[nodiscard]] std::size_t exchanges() const noexcept { return _state->exchanges.load(); }

  private:
    struct State
    {
        std::atomic<bool> stop { false };
        std::atomic<std::size_t> exchanges { 0 };
    };

    std::unique_ptr<State> _state;
    std::thread _thread;
};

[[nodiscard]] std::unique_ptr<IDescriptor> openDirectory(DescriptorFixture const& fixture, std::string path)
{
    auto opened = fixture.run(fixture.root().openAt(
        SymlinkPolicy::NoFollow, std::move(path), OpenFlags::Directory, DescriptorFlags::Read));
    REQUIRE(opened.has_value());
    return std::move(*opened);
}

/// @return Whether @p answer is @p expected, or Busy, the one refusal a walk by name may give when a
///         rename could have changed it.
[[nodiscard]] bool isExpectedOrBusy(FsResult<Relation> const& answer, Relation expected)
{
    if (answer)
        return *answer == expected;
    return answer.error().code == FsErrorCode::Busy;
}

/// @return What @p answer is, for a case to name what it did not expect.
[[nodiscard]] std::string describe(FsResult<Relation> const& answer)
{
    if (!answer)
        return answer.error().toString();
    constexpr auto Names =
        std::array<char const*, 4> { "Same", "FirstContainsSecond", "SecondContainsFirst", "Unrelated" };
    return Names.at(static_cast<std::size_t>(*answer));
}

} // namespace

TEST_CASE("relation under a swap between check and use", "[fs][native][relation][threading]")
{
    // p and c trade names beneath the root: each stays beneath the root and beside the other,
    // whatever name it has. The renamed directories are the opened ones, with nothing open beneath
    // them, because Windows renames no directory that has a handle open beneath it.
    if (core::fs::test::runsUnderWine())
        SKIP("wine cannot open the volume GUID paths its GetFinalPathNameByHandleW hands out, and names a "
             "renamed handle by the path it was opened by");
    auto fixture = DescriptorFixture::createNative();
    std::filesystem::create_directories(fixture.hostDirectory() / "p" / "q");
    std::filesystem::create_directories(fixture.hostDirectory() / "c");
    auto const p = openDirectory(fixture, "p");
    auto const c = openDirectory(fixture, "c");

    auto const& host = fixture.hostDirectory();
    auto const exchanger = Renamer { { { .from = host / "p", .to = host / "parked" },
                                       { .from = host / "c", .to = host / "p" },
                                       { .from = host / "parked", .to = host / "c" } },
                                     std::chrono::microseconds { 0 } };
    REQUIRE(exchanger.started());
    auto const before = exchanger.exchanges();

    auto wrong = std::size_t { 0 };
    auto firstWrong = std::string {};
    for ([[maybe_unused]] auto const round: std::views::iota(0, 300))
    {
        auto const rootP = fixture.run(fixture.fileSystem().relation(fixture.root(), *p));
        auto const cRoot = fixture.run(fixture.fileSystem().relation(*c, fixture.root()));
        auto const pC = fixture.run(fixture.fileSystem().relation(*p, *c));
        if (!isExpectedOrBusy(rootP, Relation::FirstContainsSecond)
            || !isExpectedOrBusy(cRoot, Relation::SecondContainsFirst)
            || !isExpectedOrBusy(pC, Relation::Unrelated))
        {
            if (wrong == 0)
                firstWrong =
                    "root,p: " + describe(rootP) + "; c,root: " + describe(cRoot) + "; p,c: " + describe(pC);
            ++wrong;
        }
    }
    INFO("the first unexpected round: " << firstWrong);
    CHECK(wrong == 0);
    // The renames went on while the relations were walked.
    CHECK(exchanger.exchanges() > before);
}

TEST_CASE("relation under a move to another parent", "[fs][native][relation][threading]")
{
    // a moves between x and y while relations are walked: beneath the root it always is, beneath z
    // never, and beneath x or y as it was at some instant -- never in a chain of parents no tree held.
    // a is the opened directory itself: Windows moves no directory with a handle open beneath it.
    if (core::fs::test::runsUnderWine())
        SKIP("wine cannot open the volume GUID paths its GetFinalPathNameByHandleW hands out, and names a "
             "renamed handle by the path it was opened by");
    auto fixture = DescriptorFixture::createNative();
    auto const& host = fixture.hostDirectory();
    std::filesystem::create_directories(host / "x" / "a");
    std::filesystem::create_directories(host / "y");
    std::filesystem::create_directories(host / "z");
    auto const x = openDirectory(fixture, "x");
    auto const y = openDirectory(fixture, "y");
    auto const z = openDirectory(fixture, "z");
    auto const a = openDirectory(fixture, "x/a");

    // Each move stands a few milliseconds: long enough for a walk by name (Windows) to see a still
    // tree between moves, short enough that the walks still meet the moves.
    auto const mover = Renamer { { { .from = host / "x" / "a", .to = host / "y" / "a" },
                                   { .from = host / "y" / "a", .to = host / "x" / "a" } },
                                 std::chrono::milliseconds { 3 } };
    REQUIRE(mover.started());
    auto const before = mover.exchanges();

    auto wrong = std::size_t { 0 };
    auto firstWrong = std::string {};
    auto beneathY = std::size_t { 0 };
    // Bounded by count, not by wall time: at least 300 rounds, and on until a walk has found a
    // beneath y, where it was never opened.
    for (auto const round: std::views::iota(0, 20000))
    {
        if (round >= 300 && beneathY > 0)
            break;
        auto const fromRoot = fixture.run(fixture.fileSystem().relation(fixture.root(), *a));
        auto const fromZ = fixture.run(fixture.fileSystem().relation(*z, *a));
        auto const fromX = fixture.run(fixture.fileSystem().relation(*x, *a));
        auto const fromY = fixture.run(fixture.fileSystem().relation(*y, *a));
        if (!isExpectedOrBusy(fromRoot, Relation::FirstContainsSecond)
            || !isExpectedOrBusy(fromZ, Relation::Unrelated)
            || !(isExpectedOrBusy(fromX, Relation::FirstContainsSecond)
                 || isExpectedOrBusy(fromX, Relation::Unrelated))
            || !(isExpectedOrBusy(fromY, Relation::FirstContainsSecond)
                 || isExpectedOrBusy(fromY, Relation::Unrelated)))
        {
            if (wrong == 0)
                firstWrong = "root: " + describe(fromRoot) + "; z: " + describe(fromZ)
                             + "; x: " + describe(fromX) + "; y: " + describe(fromY);
            ++wrong;
        }
        if (fromY && *fromY == Relation::FirstContainsSecond)
            ++beneathY;
    }
    INFO("the first unexpected round: " << firstWrong);
    CHECK(wrong == 0);
    CHECK(mover.exchanges() > before);
    // a was opened beneath x: a walk that found it beneath y followed it to where it is now, which
    // is what shows the walks ran while it moved and named its parent of that instant.
    CHECK(beneathY > 0);
}
