// SPDX-License-Identifier: Apache-2.0
//
// No path reaches outside the directory it is resolved beneath: every *At operation, with and
// without following, against every way a path can try to leave (a link up, an absolute link, `..`
// past the root, a final link to `..`), under both strategies (openat2 and the component walk on
// Linux) and both kinds. An outside decoy sits where each escape would land, and every row checks
// it was left alone. Then the race: a directory swapped for an outward link while paths through it
// are resolved.
#include <core/fs/Descriptor.hpp>
#include <core/fs/DescriptorTestSupport.hpp>
#include <core/fs/FsError.hpp>
#include <core/fs/NativeFileSystem.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

using core::async::Task;
using core::fs::BeneathStrategy;
using core::fs::DescriptorFlags;
using core::fs::FileTime;
using core::fs::FsErrorCode;
using core::fs::OpenFlags;
using core::fs::SymlinkPolicy;
using core::fs::test::DescriptorFixture;
using core::fs::test::DirectoryLinkNature;
using core::fs::test::FixtureKind;
using core::fs::test::NativeDirectoryLinkNature;
using core::fs::test::Outcome;
using core::fs::test::outcomeOf;
using core::fs::test::SymlinkSupport;

namespace
{

/// A file system to run a case over: a kind, and for a native one the strategy it resolves with.
struct Variant
{
    FixtureKind kind;
    BeneathStrategy strategy;
};

/// Both kinds, with both strategies for the native one.
std::vector<Variant> variants()
{
    auto all = std::vector<Variant> {};
    for (auto const kind: { FixtureKind::InMemory, FixtureKind::Native })
    {
        if (kind == FixtureKind::InMemory)
            all.push_back({ .kind = kind, .strategy = BeneathStrategy::Best });
        else
            for (auto const strategy: { BeneathStrategy::Best, BeneathStrategy::ComponentWalk })
                all.push_back({ .kind = kind, .strategy = strategy });
    }
    return all;
}

std::string_view nameOf(Variant const& variant)
{
    if (variant.kind == FixtureKind::InMemory)
        return "in-memory";
    return variant.strategy == BeneathStrategy::Best ? "native, Best" : "native, ComponentWalk";
}

/// How a path tries to leave the root.
enum class Shape : std::uint8_t
{
    UpLink,       ///< `up/<name>`, where `up` links to `../`.
    AbsoluteLink, ///< `abs/<name>`, where `abs` links to the outside directory's absolute path.
    TrailingUp,   ///< `a/../..`: the parent of the root itself.
    ClimbBeyond,  ///< `a/../../<name>`.
    FinalLinkUp,  ///< `uplink`, a link to `..`, as the final component.
    FinalLinkOut, ///< `outlink`, a link to the outside file `../decoy`, as the final component.
};

constexpr auto Shapes = std::to_array({ Shape::UpLink,
                                        Shape::AbsoluteLink,
                                        Shape::TrailingUp,
                                        Shape::ClimbBeyond,
                                        Shape::FinalLinkUp,
                                        Shape::FinalLinkOut });

std::string_view nameOf(Shape shape)
{
    switch (shape)
    {
        case Shape::UpLink: return "up/<name>";
        case Shape::AbsoluteLink: return "abs/<name>";
        case Shape::TrailingUp: return "a/../..";
        case Shape::ClimbBeyond: return "a/../../<name>";
        case Shape::FinalLinkUp: return "uplink";
        case Shape::FinalLinkOut: return "outlink";
    }
    return "?";
}

/// @return The path of @p shape aimed at @p name beside the root.
std::string pathOf(Shape shape, std::string_view name)
{
    switch (shape)
    {
        case Shape::UpLink: return "up/" + std::string { name };
        case Shape::AbsoluteLink: return "abs/" + std::string { name };
        case Shape::TrailingUp: return "a/../..";
        case Shape::ClimbBeyond: return "a/../../" + std::string { name };
        case Shape::FinalLinkUp: return "uplink";
        case Shape::FinalLinkOut: return "outlink";
    }
    return {};
}

/// What one row starts: the fixture, the policy and the shaped path.
struct Call
{
    DescriptorFixture const* fixture = nullptr;
    SymlinkPolicy policy = SymlinkPolicy::NoFollow;
    std::string path;
};

/// One operation of the table.
struct EscapeOp
{
    std::string_view operation;
    std::string_view aimedAt; ///< What beside the root an escape would reach.
    Task<Outcome> (*start)(Call);
    /// What it answers where the path is `uplink` and the link is not followed (`NoFollow`, or an
    /// operation that never follows its final component): it acts on the link, inside the root.
    Outcome onFinalLink;
};

/// An explicit time no file in a fixture has, so a setTimes that escaped shows in the decoy.
constexpr auto Stamp = FileTime { .seconds = 1'000'000, .nanoseconds = 0 };

constexpr auto Ops = std::to_array<EscapeOp>({
    { .operation = "openAt(Create|Truncate)",
      .aimedAt = "decoy",
      .start = [](Call c) -> Task<Outcome> {
          co_return outcomeOf(co_await c.fixture->root().openAt(
              c.policy, c.path, OpenFlags::Create | OpenFlags::Truncate, DescriptorFlags::Write));
      },
      .onFinalLink = FsErrorCode::Loop },
    { .operation = "statAt",
      .aimedAt = "decoy",
      .start = [](Call c) -> Task<Outcome> {
          co_return outcomeOf(co_await c.fixture->root().statAt(c.policy, c.path));
      },
      .onFinalLink = std::nullopt },
    { .operation = "setTimesAt",
      .aimedAt = "decoy",
      .start = [](Call c) -> Task<Outcome> {
          co_return outcomeOf(co_await c.fixture->root().setTimesAt(c.policy, c.path, Stamp, Stamp));
      },
      .onFinalLink = std::nullopt },
    { .operation = "linkAt(from)",
      .aimedAt = "decoy",
      .start = [](Call c) -> Task<Outcome> {
          auto& root = c.fixture->root();
          co_return outcomeOf(co_await root.linkAt(c.policy, c.path, &root, "fresh"));
      },
      .onFinalLink = std::nullopt },
    { .operation = "linkAt(to)",
      .aimedAt = "fresh",
      .start = [](Call c) -> Task<Outcome> {
          auto& root = c.fixture->root();
          co_return outcomeOf(co_await root.linkAt(c.policy, "f", &root, c.path));
      },
      .onFinalLink = FsErrorCode::Exist },
    { .operation = "readlinkAt",
      .aimedAt = "decoy",
      .start = [](Call c) -> Task<Outcome> {
          co_return outcomeOf(co_await c.fixture->root().readlinkAt(c.path));
      },
      .onFinalLink = std::nullopt },
    { .operation = "removeDirectoryAt",
      .aimedAt = "dir",
      .start = [](Call c) -> Task<Outcome> {
          co_return outcomeOf(co_await c.fixture->root().removeDirectoryAt(c.path));
      },
      .onFinalLink = FsErrorCode::NotDirectory },
    { .operation = "renameAt(from)",
      .aimedAt = "decoy",
      .start = [](Call c) -> Task<Outcome> {
          auto& root = c.fixture->root();
          co_return outcomeOf(co_await root.renameAt(c.path, &root, "fresh"));
      },
      .onFinalLink = std::nullopt },
    { .operation = "renameAt(to)",
      .aimedAt = "decoy",
      .start = [](Call c) -> Task<Outcome> {
          auto& root = c.fixture->root();
          co_return outcomeOf(co_await root.renameAt("f", &root, c.path));
      },
      .onFinalLink = std::nullopt },
    { .operation = "symlinkAt",
      .aimedAt = "fresh",
      .start = [](Call c) -> Task<Outcome> {
          co_return outcomeOf(co_await c.fixture->root().symlinkAt("../", c.path));
      },
      .onFinalLink = FsErrorCode::Exist },
    { .operation = "unlinkFileAt",
      .aimedAt = "decoy",
      .start = [](Call c) -> Task<Outcome> {
          co_return outcomeOf(co_await c.fixture->root().unlinkFileAt(c.path));
      },
      .onFinalLink = std::nullopt },
    { .operation = "metadataHashAt",
      .aimedAt = "decoy",
      .start = [](Call c) -> Task<Outcome> {
          co_return outcomeOf(co_await c.fixture->root().metadataHashAt(c.policy, c.path));
      },
      .onFinalLink = std::nullopt },
    { .operation = "createDirectoryAt",
      .aimedAt = "fresh",
      .start = [](Call c) -> Task<Outcome> {
          co_return outcomeOf(
              co_await c.fixture->root().createDirectoryAt(c.path, core::fs::CreationAccess::Default));
      },
      .onFinalLink = FsErrorCode::Exist },
});

/// @return Whether @p op takes a @c SymlinkPolicy for its final component.
bool takesPolicy(EscapeOp const& op)
{
    constexpr auto WithPolicy = std::to_array<std::string_view>({ "openAt(Create|Truncate)",
                                                                  "statAt",
                                                                  "setTimesAt",
                                                                  "linkAt(from)",
                                                                  "linkAt(to)",
                                                                  "metadataHashAt" });
    return std::ranges::find(WithPolicy, op.operation) != WithPolicy.end();
}

/// @return Whether @p op is one of the two `linkAt` rows, which refuse `Follow` as `Invalid`.
bool isLinkAt(EscapeOp const& op)
{
    return op.operation.starts_with("linkAt");
}

/// What a row must answer (decision D4, ruling R9).
Outcome expectedOf(EscapeOp const& op, Variant const& variant, SymlinkPolicy policy, Shape shape)
{
    if (isLinkAt(op) && policy == SymlinkPolicy::Follow)
        return FsErrorCode::Invalid;
    if ((shape == Shape::FinalLinkUp || shape == Shape::FinalLinkOut)
        && (policy == SymlinkPolicy::NoFollow || !takesPolicy(op)))
    {
        // `uplink` names a directory, so Windows made it a directory link, which is a directory to
        // the file system: no hard link names it, and no file replaces it.
        if ((op.operation == "linkAt(from)" || op.operation == "renameAt(to)") && shape == Shape::FinalLinkUp
            && variant.kind == FixtureKind::Native
            && NativeDirectoryLinkNature == DirectoryLinkNature::Directory)
            return FsErrorCode::IsDirectory;
        return op.onFinalLink;
    }
    return FsErrorCode::NotPermitted;
}

/// A fixture with `a/`, `f`, `up -> ../`, `uplink -> ..`, `outlink -> ../decoy` and `abs -> <outside>`
/// beneath the root, and the decoys `decoy` (a file) and `dir` (a directory) beside it.
DescriptorFixture escapeFixture(Variant const& variant)
{
    auto fixture = DescriptorFixture::create(variant.kind, { .strategy = variant.strategy });
    if (fixture.requireSymlinks() == SymlinkSupport::Unavailable)
        return fixture;
    REQUIRE(
        fixture.run(fixture.root().createDirectoryAt("a", core::fs::CreationAccess::Default)).has_value());
    fixture.createFile("f", "inside");
    REQUIRE(fixture.run(fixture.root().symlinkAt("../", "up")).has_value());
    REQUIRE(fixture.run(fixture.root().symlinkAt("..", "uplink")).has_value());
    REQUIRE(fixture.run(fixture.root().symlinkAt("../decoy", "outlink")).has_value());
    fixture.hostSymlink("abs", fixture.outsideAbsoluteTarget());
    fixture.writeOutsideFile("decoy", "outside");
    fixture.createOutsideDirectory("dir");
    return fixture;
}

} // namespace

TEST_CASE("no *At operation reaches outside the preopen", "[fs][escape]")
{
    auto const variant = GENERATE(from_range(variants()));
    auto const shape = GENERATE(from_range(Shapes));
    auto const policy = GENERATE(SymlinkPolicy::NoFollow, SymlinkPolicy::Follow);
    CAPTURE(nameOf(variant), nameOf(shape), policy == SymlinkPolicy::Follow ? "Follow" : "NoFollow");

    for (auto const& op: Ops)
    {
        CAPTURE(op.operation);
        auto fixture = escapeFixture(variant);
        if (fixture.requireSymlinks() == SymlinkSupport::Unavailable)
            SKIP("this file system cannot create symbolic links");
        auto const outcome = fixture.run(
            op.start(Call { .fixture = &fixture, .policy = policy, .path = pathOf(shape, op.aimedAt) }));
        CHECK(outcome == expectedOf(op, variant, policy, shape));
        CHECK(fixture.outsideUnchanged());
        CHECK(outcomeOf(fixture.run(fixture.outside().statAt(SymlinkPolicy::NoFollow, "fresh")))
              == FsErrorCode::NoEntry);
    }
}

TEST_CASE("a symlink cycle ends in Loop, not a hang", "[fs][escape]")
{
    auto const variant = GENERATE(from_range(variants()));
    CAPTURE(nameOf(variant));
    auto fixture = DescriptorFixture::create(variant.kind, { .strategy = variant.strategy });
    if (fixture.requireSymlinks() == SymlinkSupport::Unavailable)
        SKIP("this file system cannot create symbolic links");
    auto& root = fixture.root();
    REQUIRE(fixture.run(root.symlinkAt("pong", "ping")).has_value());
    REQUIRE(fixture.run(root.symlinkAt("ping/", "pong")).has_value());

    CHECK(outcomeOf(fixture.run(root.statAt(SymlinkPolicy::Follow, "ping"))) == FsErrorCode::Loop);
    CHECK(outcomeOf(fixture.run(root.statAt(SymlinkPolicy::NoFollow, "ping/x"))) == FsErrorCode::Loop);
    CHECK(outcomeOf(fixture.run(root.createDirectoryAt("pong/new", core::fs::CreationAccess::Default)))
          == FsErrorCode::Loop);
    CHECK(outcomeOf(fixture.run(
              root.openAt(SymlinkPolicy::NoFollow, "ping/f", OpenFlags::Create, DescriptorFlags::Write)))
          == FsErrorCode::Loop);
    CHECK(fixture.run(root.statAt(SymlinkPolicy::NoFollow, "ping")).has_value());
}

TEST_CASE("a directory swapped for an outward symlink mid-walk never yields a descriptor outside the preopen",
          "[fs][escape]")
{
    auto const strategy = GENERATE(BeneathStrategy::Best, BeneathStrategy::ComponentWalk);
    CAPTURE(strategy == BeneathStrategy::Best ? "Best" : "ComponentWalk");
    auto fixture = DescriptorFixture::createNative({ .strategy = strategy });
    if (fixture.requireSymlinks() == SymlinkSupport::Unavailable)
        SKIP("this file system cannot create symbolic links");
    fixture.writeFile("d/inside.txt", "inside");
    fixture.writeOutsideFile("inside.txt", "outside"); // the decoy at the escape target
    auto openedInside = 0;
    auto refused = 0;
    {
        auto const swapper = fixture.startSwapper("d", "../");
        // Bounded by count, not by wall time: at least 2000 rounds, and on until the swapper has made
        // its hundred swaps, which a slow runner (macOS CI: 78 in 2000) needs more rounds for.
        for (auto const round: std::views::iota(0, 20'000))
        {
            if (round >= 2000 && swapper->swaps() > 100)
                break;
            INFO(round);
            auto opened = fixture.run(fixture.root().openAt(
                SymlinkPolicy::Follow, "d/inside.txt", OpenFlags::None, DescriptorFlags::Read));
            if (opened)
            {
                REQUIRE(fixture.isInsideRoot(**opened));
                ++openedInside;
            }
            else
                ++refused;
            // The decoy holds "outside", one byte more than "inside": a stat that escaped shows it.
            if (auto const status = fixture.run(fixture.root().statAt(SymlinkPolicy::Follow, "d/inside.txt")))
                CHECK(status->size == 6);
            std::ignore =
                fixture.run(fixture.root().setTimesAt(SymlinkPolicy::Follow, "d/inside.txt", Stamp, Stamp));
        }
        UNSCOPED_INFO("swaps: " << swapper->swaps());
        CHECK(swapper->swaps() > 100);
    }
    CHECK(openedInside > 0);
    CHECK(refused > 0);
    CHECK(fixture.outsideUnchanged());
}
