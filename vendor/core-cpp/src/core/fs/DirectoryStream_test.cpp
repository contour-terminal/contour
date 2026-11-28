// SPDX-License-Identifier: Apache-2.0
//
// What a directory stream and object identity owe their caller, whichever file system is under them:
// every entry once, typed, never "." or ".."; a stream of its own per readDirectory, holding one
// batch rather than the listing; isSameObject by identity; metadataHashAt as metadataHash. Each case
// GENERATEs the fixture kinds it holds for.
#include <core/fs/Descriptor.hpp>
#include <core/fs/DescriptorTestSupport.hpp>
#include <core/fs/FsError.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using core::fs::DescriptorFlags;
using core::fs::DescriptorType;
using core::fs::DirectoryEntry;
using core::fs::FsErrorCode;
using core::fs::FsResult;
using core::fs::IDescriptor;
using core::fs::IDirectoryStream;
using core::fs::OpenFlags;
using core::fs::SymlinkPolicy;
using core::fs::test::DescriptorFixture;
using core::fs::test::FixtureKind;
using core::fs::test::outcomeOf;
using core::fs::test::SymlinkSupport;

namespace
{

constexpr auto FileCount = std::size_t { 300 };

/// The most answers any case's directory holds, with room to spare: one more is a stream that does
/// not end.
constexpr auto MaxAnswers = std::size_t { 4'000 };

/// How many rounds of draining a hand-started call gets before it is called stuck.
constexpr auto MaxRounds = DescriptorFixture::MaxRounds;

std::string fileName(std::size_t index)
{
    return std::format("file-{:03}", index);
}

/// How many files the cases that need several batches of every kind make.
constexpr auto LongCount = std::size_t { 1'000 };

/// The length of their names: 1000 of them spell more than two 64 KiB batches (Linux's getdents64,
/// Windows' FileIdBothDirectoryInfo) in name bytes alone, and in-memory and readdir batches count 64
/// entries, so every kind needs at least three batches, and a hop for each.
constexpr auto LongNameLength = std::size_t { 155 };

/// @return The @p index-th of @c LongCount names, each @c LongNameLength bytes: long enough for
///         several batches, short enough for any file system and Windows' MAX_PATH.
std::string longName(std::size_t index)
{
    return std::format("{}-{:04}", std::string(LongNameLength - 5, 'n'), index);
}

/// Fills the root with @c FileCount files and the directory `sub`.
void populate(DescriptorFixture const& fixture)
{
    for (auto const index: std::views::iota(std::size_t { 0 }, FileCount))
        fixture.createFile(fileName(index), "x");
    REQUIRE(
        fixture.run(fixture.root().createDirectoryAt("sub", core::fs::CreationAccess::Default)).has_value());
}

std::unique_ptr<IDirectoryStream> openStream(DescriptorFixture const& fixture, IDescriptor& directory)
{
    auto stream = fixture.run(directory.readDirectory());
    REQUIRE(stream.has_value());
    return std::move(*stream);
}

/// @return The names of @p answers, each with how often it came; fails the case on an answer that
///         is no entry.
std::map<std::string, std::size_t> countNames(std::vector<FsResult<DirectoryEntry>> const& answers)
{
    auto counts = std::map<std::string, std::size_t> {};
    for (auto const& answer: answers)
    {
        REQUIRE(answer.has_value());
        ++counts[answer->name];
    }
    return counts;
}

std::unique_ptr<IDescriptor> openAt(DescriptorFixture const& fixture, std::string path, DescriptorFlags flags)
{
    auto opened =
        fixture.run(fixture.root().openAt(SymlinkPolicy::NoFollow, std::move(path), OpenFlags::None, flags));
    REQUIRE(opened.has_value());
    return std::move(*opened);
}

} // namespace

TEST_CASE("directory stream: every entry once, typed, without . and ..", "[fs][contract][directory]")
{
    auto fixture = DescriptorFixture::create(GENERATE(FixtureKind::InMemory, FixtureKind::Native));
    populate(fixture);

    auto stream = openStream(fixture, fixture.root());
    auto const answers = fixture.readEntries(*stream, MaxAnswers);
    REQUIRE(answers.size() == FileCount + 1);
    auto const counts = countNames(answers);
    CHECK(counts.size() == FileCount + 1);
    CHECK_FALSE(counts.contains("."));
    CHECK_FALSE(counts.contains(".."));
    for (auto const& answer: answers)
    {
        CAPTURE(answer->name);
        CHECK(counts.at(answer->name) == 1);
        auto const expected = answer->name == "sub" ? DescriptorType::Directory : DescriptorType::RegularFile;
        CHECK(answer->type == expected);
    }
    CHECK(counts.contains(fileName(0)));
    CHECK(counts.contains(fileName(FileCount - 1)));

    // The end is the end: asked again, the stream has nothing more.
    auto const after = fixture.run(stream->next());
    REQUIRE(after.has_value());
    CHECK(*after == std::nullopt);
}

TEST_CASE("directory stream: a symbolic link is typed as one, never followed", "[fs][contract][directory]")
{
    auto fixture = DescriptorFixture::create(GENERATE(FixtureKind::InMemory, FixtureKind::Native));
    if (fixture.requireSymlinks() == SymlinkSupport::Unavailable)
        SKIP("this file system cannot create symbolic links");
    fixture.createFile("f", "x");
    REQUIRE(
        fixture.run(fixture.root().createDirectoryAt("d", core::fs::CreationAccess::Default)).has_value());
    REQUIRE(fixture.run(fixture.root().symlinkAt("f", "to-file")).has_value());
    REQUIRE(fixture.run(fixture.root().symlinkAt("d", "to-directory")).has_value());
    REQUIRE(fixture.run(fixture.root().symlinkAt("missing", "dangling")).has_value());

    auto stream = openStream(fixture, fixture.root());
    auto types = std::map<std::string, DescriptorType> {};
    for (auto const& answer: fixture.readEntries(*stream, MaxAnswers))
    {
        REQUIRE(answer.has_value());
        types.emplace(answer->name, answer->type);
    }
    CHECK(types
          == std::map<std::string, DescriptorType> {
              { "d", DescriptorType::Directory },
              { "dangling", DescriptorType::SymbolicLink },
              { "f", DescriptorType::RegularFile },
              { "to-directory", DescriptorType::SymbolicLink },
              { "to-file", DescriptorType::SymbolicLink },
          });
}

TEST_CASE("directory stream: each stream reads the directory from its start", "[fs][contract][directory]")
{
    auto fixture = DescriptorFixture::create(GENERATE(FixtureKind::InMemory, FixtureKind::Native));
    populate(fixture);

    SECTION("two consecutive streams each yield every entry")
    {
        for ([[maybe_unused]] auto const round: { 1, 2 })
        {
            auto stream = openStream(fixture, fixture.root());
            auto const answers = fixture.readEntries(*stream, MaxAnswers);
            CHECK(answers.size() == FileCount + 1);
            CHECK(countNames(answers).size() == FileCount + 1);
        }
    }
    SECTION("two interleaved streams together yield each name exactly twice")
    {
        auto first = openStream(fixture, fixture.root());
        auto second = openStream(fixture, fixture.root());
        auto counts = std::map<std::string, std::size_t> {};
        auto ended = std::set<IDirectoryStream*> {};
        auto answers = std::size_t { 0 };
        while (ended.size() < 2 && answers <= MaxAnswers)
        {
            for (auto* const stream: { first.get(), second.get() })
            {
                if (ended.contains(stream))
                    continue;
                auto next = fixture.run(stream->next());
                REQUIRE(next.has_value());
                if (!*next)
                {
                    ended.insert(stream);
                    continue;
                }
                ++answers;
                ++counts[(*next)->name];
            }
        }
        REQUIRE(ended.size() == 2);
        CHECK(answers == 2 * (FileCount + 1));
        CHECK(counts.size() == FileCount + 1);
        CHECK(std::ranges::all_of(counts, [](auto const& count) { return count.second == 2; }));
    }
}

TEST_CASE("directory stream: the stream holds a batch, never the whole listing", "[fs][contract][directory]")
{
    auto fixture = DescriptorFixture::create(GENERATE(FixtureKind::InMemory, FixtureKind::Native));
    constexpr auto Count = LongCount;
    for (auto const index: std::views::iota(std::size_t { 0 }, Count))
        fixture.createFile(longName(index), "");

    auto stream = openStream(fixture, fixture.root());
    // The setup's own closes are queued on blocking; they are not the stream's.
    fixture.blocking().drain();
    fixture.home().drain();
    auto hops = std::size_t { 0 };
    auto entries = std::size_t { 0 };
    while (entries <= Count)
    {
        // Started by hand rather than through run(), so a call answered from the batch in hand is
        // seen to complete without a hop.
        auto task = stream->next();
        task.handle().resume();
        fixture.home().drain();
        if (!task.done())
        {
            ++hops;
            for ([[maybe_unused]] auto const round: std::views::iota(std::size_t { 0 }, MaxRounds))
                if (task.done() || fixture.blocking().drain() + fixture.home().drain() == 0)
                    break;
        }
        if (!task.done())
        {
            // Leaked rather than freed while an executor may still point into it, as run() does.
            std::ignore = task.release();
            FAIL("next() was still suspended after " << MaxRounds << " rounds of draining both executors");
        }
        auto const next = task.result();
        REQUIRE(next.has_value());
        if (!*next)
            break;
        ++entries;
    }
    CHECK(entries == Count);
    static_assert(LongCount * LongNameLength > std::size_t { 2 } * 64 * 1024);
    static_assert(LongCount > std::size_t { 2 } * 64);
    CHECK(hops >= 3);
    CHECK(hops <= Count / 2);
}

TEST_CASE("directory stream: a directory changed while it is read is read to an end",
          "[fs][contract][directory]")
{
    auto fixture = DescriptorFixture::create(GENERATE(FixtureKind::InMemory, FixtureKind::Native));
    // Enough for several batches of every kind, so the changes land both in the batch in hand and
    // in batches the system has not read yet.
    for (auto const index: std::views::iota(std::size_t { 0 }, LongCount))
        fixture.createFile(longName(index), "x");
    REQUIRE(
        fixture.run(fixture.root().createDirectoryAt("sub", core::fs::CreationAccess::Default)).has_value());
    auto stream = openStream(fixture, fixture.root());

    // Every tenth file is removed, unless the stream has handed it out already, and as many new ones
    // are made, one after each entry read.
    auto removed = std::set<std::string> {};
    for (auto const index: std::views::iota(std::size_t { 0 }, LongCount))
        if (index % 10 == 9)
            removed.insert(longName(index));
    auto counts = std::map<std::string, std::size_t> {};
    auto added = std::size_t { 0 };
    auto answers = std::size_t { 0 };
    while (answers <= MaxAnswers)
    {
        auto next = fixture.run(stream->next());
        REQUIRE(next.has_value());
        if (!*next)
            break;
        ++answers;
        ++counts[(*next)->name];
        if (added < removed.size())
        {
            auto const victim = std::next(removed.begin(), static_cast<std::ptrdiff_t>(added));
            if (!counts.contains(*victim))
                REQUIRE(fixture.run(fixture.root().unlinkFileAt(*victim)).has_value());
            fixture.createFile(std::format("new-{:04}", added), "y");
            ++added;
        }
    }
    REQUIRE(answers <= MaxAnswers);
    CHECK(added == removed.size());
    CHECK(std::ranges::all_of(counts, [](auto const& count) { return count.second == 1; }));
    // What was there throughout and never removed came exactly once.
    CHECK(counts.contains("sub"));
    auto missing = std::size_t { 0 };
    for (auto const index: std::views::iota(std::size_t { 0 }, LongCount))
        if (!removed.contains(longName(index)) && !counts.contains(longName(index)))
            ++missing;
    CHECK(missing == 0);
}

TEST_CASE("directory stream: a stream outlives the descriptor it was opened from",
          "[fs][contract][directory]")
{
    auto fixture = DescriptorFixture::create(GENERATE(FixtureKind::InMemory, FixtureKind::Native));
    fixture.writeFile("sub/a", "x");
    fixture.writeFile("sub/b", "x");
    auto sub = openAt(fixture, "sub", DescriptorFlags::Read);
    auto stream = openStream(fixture, *sub);
    sub.reset();
    auto const counts = countNames(fixture.readEntries(*stream, MaxAnswers));
    CHECK(counts == std::map<std::string, std::size_t> { { "a", 1 }, { "b", 1 } });
}

TEST_CASE("directory stream: a file has no entries to read", "[fs][contract][directory]")
{
    auto fixture = DescriptorFixture::create(GENERATE(FixtureKind::InMemory, FixtureKind::Native));
    fixture.createFile("f", "x");
    auto file = openAt(fixture, "f", DescriptorFlags::Read);
    CHECK(outcomeOf(fixture.run(file->readDirectory())) == FsErrorCode::NotDirectory);
}

TEST_CASE("isSameObject: two descriptors of one object are the same object", "[fs][contract][identity]")
{
    auto fixture = DescriptorFixture::create(GENERATE(FixtureKind::InMemory, FixtureKind::Native));
    fixture.createFile("f", "x");
    fixture.createFile("g", "x");
    auto const first = openAt(fixture, "f", DescriptorFlags::Read);
    auto const second = openAt(fixture, "f", DescriptorFlags::None);
    auto const other = openAt(fixture, "g", DescriptorFlags::Read);

    CHECK(fixture.run(first->isSameObject(second.get())) == true);
    CHECK(fixture.run(second->isSameObject(first.get())) == true);
    CHECK(fixture.run(first->isSameObject(first.get())) == true);
    CHECK(fixture.run(first->isSameObject(other.get())) == false);
    CHECK(fixture.run(first->isSameObject(&fixture.root())) == false);

    auto root = fixture.openRoot(DescriptorFlags::Read);
    REQUIRE(root.has_value());
    CHECK(fixture.run((*root)->isSameObject(&fixture.root())) == true);
    CHECK(fixture.run(fixture.root().isSameObject(&fixture.outside())) == false);

    // A hard link names the same object.
    REQUIRE(fixture.run(fixture.root().linkAt(SymlinkPolicy::NoFollow, "f", &fixture.root(), "hard"))
                .has_value());
    auto const hard = openAt(fixture, "hard", DescriptorFlags::Read);
    CHECK(fixture.run(hard->isSameObject(first.get())) == true);

    CHECK(outcomeOf(fixture.run(first->isSameObject(nullptr))) == FsErrorCode::BadDescriptor);
}

TEST_CASE("isSameObject: a descriptor of another file system is never the same object",
          "[fs][contract][identity]")
{
    auto const kind = GENERATE(FixtureKind::InMemory, FixtureKind::Native);
    auto fixture = DescriptorFixture::create(kind);
    fixture.createFile("f", "x");
    auto const mine = openAt(fixture, "f", DescriptorFlags::Read);

    // Another instance of the same kind, and one of the other kind.
    auto same = kind == FixtureKind::Native ? fixture.sibling(DescriptorFixture::DefaultSecretSeed)
                                            : DescriptorFixture::create(FixtureKind::InMemory);
    if (kind == FixtureKind::InMemory)
        same.createFile("f", "x");
    auto const theirs = openAt(same, "f", DescriptorFlags::Read);
    CHECK(fixture.run(mine->isSameObject(theirs.get())) == false);

    auto foreign =
        DescriptorFixture::create(kind == FixtureKind::Native ? FixtureKind::InMemory : FixtureKind::Native);
    CHECK(fixture.run(mine->isSameObject(&foreign.root())) == false);
}

TEST_CASE("metadataHashAt: a link is hashed itself, or what it names where followed",
          "[fs][contract][identity]")
{
    auto fixture = DescriptorFixture::create(GENERATE(FixtureKind::InMemory, FixtureKind::Native));
    if (fixture.requireSymlinks() == SymlinkSupport::Unavailable)
        SKIP("this file system cannot create symbolic links");
    fixture.createFile("f", "x");
    REQUIRE(fixture.run(fixture.root().symlinkAt("f", "link")).has_value());
    auto const target = fixture.run(fixture.root().metadataHashAt(SymlinkPolicy::NoFollow, "f"));
    auto const followed = fixture.run(fixture.root().metadataHashAt(SymlinkPolicy::Follow, "link"));
    auto const itself = fixture.run(fixture.root().metadataHashAt(SymlinkPolicy::NoFollow, "link"));
    REQUIRE(target.has_value());
    REQUIRE(followed.has_value());
    REQUIRE(itself.has_value());
    CHECK(*followed == *target);
    CHECK(*itself != *target);
}

TEST_CASE("metadataHashAt: equals the hash of a descriptor of the same object", "[fs][contract][identity]")
{
    auto fixture = DescriptorFixture::create(GENERATE(FixtureKind::InMemory, FixtureKind::Native));
    fixture.createFile("f", "x");
    REQUIRE(
        fixture.run(fixture.root().createDirectoryAt("d", core::fs::CreationAccess::Default)).has_value());
    for (auto const* name: { "f", "d" })
    {
        CAPTURE(name);
        auto const opened = openAt(fixture, name, DescriptorFlags::Read);
        auto const byDescriptor = fixture.run(opened->metadataHash());
        auto const byPath = fixture.run(fixture.root().metadataHashAt(SymlinkPolicy::NoFollow, name));
        REQUIRE(byDescriptor.has_value());
        REQUIRE(byPath.has_value());
        CHECK(*byDescriptor == *byPath);
    }
    auto const file = fixture.run(fixture.root().metadataHashAt(SymlinkPolicy::NoFollow, "f"));
    auto const directory = fixture.run(fixture.root().metadataHashAt(SymlinkPolicy::NoFollow, "d"));
    REQUIRE(file.has_value());
    REQUIRE(directory.has_value());
    CHECK(*file != *directory);
}
