// SPDX-License-Identifier: Apache-2.0
//
// What only the native descriptors owe: no system call on the caller's executor, a handle that
// outlives every call still parked on it, and a metadata hash keyed by the file system's secret.
#include <core/async/Task.hpp>
#include <core/fs/Descriptor.hpp>
#include <core/fs/DescriptorTestSupport.hpp>
#include <core/fs/FsError.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

using core::fs::Advice;
using core::fs::DescriptorFlags;
using core::fs::FsErrorCode;
using core::fs::IDescriptor;
using core::fs::IDirectoryStream;
using core::fs::MetadataHash;
using core::fs::OpenFlags;
using core::fs::SetToNow;
using core::fs::SymlinkPolicy;
using core::fs::test::bytesOf;
using core::fs::test::DescriptorFixture;
using core::fs::test::Outcome;
using core::fs::test::outcomeOf;
using core::fs::test::SymlinkSupport;
using core::fs::test::textOf;

namespace
{

/// What a parked operation is started on: a fixture holding `f` (a file with content), `sub` (an
/// empty directory) and, where the system has them, `link` (a symbolic link to `f`), with `file`
/// open on `f`, `Read | Write`, and `stream` open on the root, not yet read.
struct ParkContext
{
    DescriptorFixture const* fixture = nullptr;
    IDescriptor* root = nullptr;
    IDescriptor* file = nullptr;
    IDirectoryStream* stream = nullptr;
};

/// One operation, started but not driven: the task it returns.
struct ParkRow
{
    std::string_view operation;
    core::async::Task<Outcome> (*start)(ParkContext);
};

constexpr auto NoFollow = SymlinkPolicy::NoFollow;

// One row per operation the native descriptors implement, and the stream's next, whose first call
// reads a batch (ruling R6).
constexpr auto ParkRows = std::to_array<ParkRow>({
    { "read",
      [](ParkContext c) -> core::async::Task<Outcome> { co_return outcomeOf(co_await c.file->read(4, 0)); } },
    { "write",
      [](ParkContext c) -> core::async::Task<Outcome> {
          co_return outcomeOf(co_await c.file->write(bytesOf("x"), 0));
      } },
    { "append",
      [](ParkContext c) -> core::async::Task<Outcome> {
          co_return outcomeOf(co_await c.file->append(bytesOf("x")));
      } },
    { "advise",
      [](ParkContext c) -> core::async::Task<Outcome> {
          co_return outcomeOf(co_await c.file->advise(0, 0, Advice::Sequential));
      } },
    { "syncData",
      [](ParkContext c) -> core::async::Task<Outcome> { co_return outcomeOf(co_await c.file->syncData()); } },
    { "sync",
      [](ParkContext c) -> core::async::Task<Outcome> { co_return outcomeOf(co_await c.file->sync()); } },
    { "type",
      [](ParkContext c) -> core::async::Task<Outcome> { co_return outcomeOf(co_await c.file->type()); } },
    { "setSize",
      [](ParkContext c) -> core::async::Task<Outcome> { co_return outcomeOf(co_await c.file->setSize(1)); } },
    { "setTimes",
      [](ParkContext c) -> core::async::Task<Outcome> {
          co_return outcomeOf(co_await c.file->setTimes(SetToNow {}, SetToNow {}));
      } },
    { "stat",
      [](ParkContext c) -> core::async::Task<Outcome> { co_return outcomeOf(co_await c.file->stat()); } },
    { "metadataHash",
      [](ParkContext c) -> core::async::Task<Outcome> {
          co_return outcomeOf(co_await c.file->metadataHash());
      } },
    { "readDirectory",
      [](ParkContext c) -> core::async::Task<Outcome> {
          co_return outcomeOf(co_await c.root->readDirectory());
      } },
    { "IDirectoryStream::next",
      [](ParkContext c) -> core::async::Task<Outcome> { co_return outcomeOf(co_await c.stream->next()); } },
    { "openAt",
      [](ParkContext c) -> core::async::Task<Outcome> {
          co_return outcomeOf(co_await c.root->openAt(NoFollow, "f", OpenFlags::None, DescriptorFlags::Read));
      } },
    { "createDirectoryAt",
      [](ParkContext c) -> core::async::Task<Outcome> {
          co_return outcomeOf(co_await c.root->createDirectoryAt("new", core::fs::CreationAccess::Default));
      } },
    { "statAt",
      [](ParkContext c) -> core::async::Task<Outcome> {
          co_return outcomeOf(co_await c.root->statAt(NoFollow, "f"));
      } },
    { "setTimesAt",
      [](ParkContext c) -> core::async::Task<Outcome> {
          co_return outcomeOf(co_await c.root->setTimesAt(NoFollow, "f", SetToNow {}, SetToNow {}));
      } },
    { "linkAt",
      [](ParkContext c) -> core::async::Task<Outcome> {
          co_return outcomeOf(co_await c.root->linkAt(NoFollow, "f", c.root, "hard"));
      } },
    { "readlinkAt",
      [](ParkContext c) -> core::async::Task<Outcome> {
          co_return outcomeOf(co_await c.root->readlinkAt("link"));
      } },
    { "removeDirectoryAt",
      [](ParkContext c) -> core::async::Task<Outcome> {
          co_return outcomeOf(co_await c.root->removeDirectoryAt("sub"));
      } },
    { "renameAt",
      [](ParkContext c) -> core::async::Task<Outcome> {
          co_return outcomeOf(co_await c.root->renameAt("f", c.root, "renamed"));
      } },
    { "symlinkAt",
      [](ParkContext c) -> core::async::Task<Outcome> {
          co_return outcomeOf(co_await c.root->symlinkAt("f", "newlink"));
      } },
    { "unlinkFileAt",
      [](ParkContext c) -> core::async::Task<Outcome> {
          co_return outcomeOf(co_await c.root->unlinkFileAt("f"));
      } },
    { "metadataHashAt",
      [](ParkContext c) -> core::async::Task<Outcome> {
          co_return outcomeOf(co_await c.root->metadataHashAt(NoFollow, "f"));
      } },
    { "isSameObject",
      [](ParkContext c) -> core::async::Task<Outcome> {
          co_return outcomeOf(co_await c.file->isSameObject(c.root));
      } },
    { "openPreopen",
      [](ParkContext c) -> core::async::Task<Outcome> {
          co_return outcomeOf(co_await c.fixture->fileSystem().openPreopen(c.fixture->hostDirectory(),
                                                                           DescriptorFlags::Read));
      } },
});

/// Opens `name` beneath @p fixture's root with @p flags.
std::unique_ptr<IDescriptor> openExisting(DescriptorFixture const& fixture,
                                          std::string name,
                                          DescriptorFlags flags)
{
    auto file = fixture.run(fixture.root().openAt(NoFollow, std::move(name), OpenFlags::None, flags));
    REQUIRE(file.has_value());
    return std::move(*file);
}

} // namespace

TEST_CASE("native: every descriptor operation parks on the blocking executor", "[fs][native]")
{
    for (auto const& row: ParkRows)
    {
        CAPTURE(row.operation);
        auto fixture = DescriptorFixture::createNative();
        fixture.createFile("f", "content");
        REQUIRE(fixture.run(fixture.root().createDirectoryAt("sub", core::fs::CreationAccess::Default))
                    .has_value());
        if (fixture.requireSymlinks() == SymlinkSupport::Available)
            REQUIRE(fixture.run(fixture.root().symlinkAt("f", "link")).has_value());
        auto const file = openExisting(fixture, "f", DescriptorFlags::Read | DescriptorFlags::Write);
        auto stream = fixture.run(fixture.root().readDirectory());
        REQUIRE(stream.has_value());
        // The setup's own closes are queued on blocking; they are not the row's.
        fixture.blocking().drain();
        fixture.home().drain();

        auto task = row.start(ParkContext {
            .fixture = &fixture, .root = &fixture.root(), .file = file.get(), .stream = stream->get() });
        task.handle().resume();
        fixture.home().drain();
        CHECK_FALSE(task.done());
        CHECK(fixture.blocking().pending() == 1);

        fixture.blocking().drain();
        CHECK_FALSE(task.done()); // it resumed on home, not inline on the blocking executor
        CHECK(fixture.home().pending() == 1);

        fixture.home().drain();
        REQUIRE(task.done());
        auto const outcome = task.result();
        CHECK(outcome == std::nullopt);
    }
}

TEST_CASE("native: a descriptor dropped under a parked read never reads a reused handle", "[fs][native]")
{
    auto fixture = DescriptorFixture::createNative();
    fixture.createFile("a", "AAAA");
    // A second file system, with executors of its own, so `b` opens while the read is still parked.
    auto other = DescriptorFixture::createNative();
    other.createFile("b", "BBBB");

    auto a = openExisting(fixture, "a", DescriptorFlags::Read);
    fixture.blocking().drain();
    fixture.home().drain();
    auto read = a->read(4, 0);
    read.handle().resume();
    REQUIRE(fixture.blocking().pending() == 1);

    a.reset(); // the descriptor goes while its read is parked
    auto const b = openExisting(other, "b", DescriptorFlags::Read); // takes the lowest free handle

    fixture.blocking().drain();
    fixture.home().drain();
    REQUIRE(read.done());
    auto const result = read.result();
    if (result)
        CHECK(textOf(result->bytes) == "AAAA");
    else
        CHECK(result.error().code == FsErrorCode::BadDescriptor);
    CHECK(textOf(other.run(b->read(4, 0)).value().bytes) == "BBBB");
}

TEST_CASE("native: the metadata hash is keyed by the file system's secret", "[fs][native]")
{
    auto fixture = DescriptorFixture::createNative({}, 1);
    fixture.createFile("f", "content");
    auto const hashIn = [](DescriptorFixture const& over) -> MetadataHash {
        auto const file = openExisting(over, "f", DescriptorFlags::Read);
        auto const hash = over.run(file->metadataHash());
        REQUIRE(hash.has_value());
        return *hash;
    };

    auto const first = hashIn(fixture);
    CHECK(hashIn(fixture.sibling(1)) == first);
    CHECK(hashIn(fixture.sibling(2)) != first);
}

TEST_CASE("native: the last close of a handle runs on the blocking executor", "[fs][native]")
{
    auto fixture = DescriptorFixture::createNative();
    fixture.createFile("f", "content");

    SECTION("a descriptor dropped with nothing in flight")
    {
        auto file = openExisting(fixture, "f", DescriptorFlags::Read);
        fixture.blocking().drain();
        fixture.home().drain();
        REQUIRE(fixture.blocking().pending() == 0);

        file.reset();
        CHECK(fixture.home().pending() == 0);
        CHECK(fixture.blocking().pending() == 1); // the close, queued rather than made here
        fixture.blocking().drain();
        CHECK(fixture.blocking().pending() == 0);
    }
    SECTION("a descriptor dropped under a parked read")
    {
        auto file = openExisting(fixture, "f", DescriptorFlags::Read);
        fixture.blocking().drain();
        fixture.home().drain();
        {
            auto read = file->read(4, 0);
            read.handle().resume();
            file.reset();
            REQUIRE(fixture.blocking().pending() == 1); // the read, holding the handle

            fixture.blocking().drain();
            fixture.home().drain();
            REQUIRE(read.done());
            CHECK(textOf(read.result().value().bytes) == "cont");
            CHECK(fixture.blocking().pending() == 0);
        } // the read's frame, the handle's last owner, goes here, on the caller's thread
        CHECK(fixture.blocking().pending() == 1); // and the close went to blocking
    }
}

namespace
{

/// A policy that splits on `|` alone, so `/` and `\` are ordinary characters to it: an override a
/// caller could pass, and which must never let the kernel see more than one component.
constexpr auto BarPolicy = core::fs::PathPolicy {
    .separators = "|",
    .targetSeparators = "|",
    .reservedNames = {},
    .volumeSeparators = "",
    .forbiddenCharacters = "",
    .encoding = core::fs::PathEncoding::Utf8Bytes,
    .caseRule = core::fs::CaseRule::Sensitive,
    .trailing = core::fs::TrailingDotsAndSpaces::Allowed,
};

} // namespace

TEST_CASE("native: a name never holds what the kernel splits on, whatever the policy", "[fs][native]")
{
    auto fixture = DescriptorFixture::createNative({ .policy = &BarPolicy });
    REQUIRE(
        fixture.run(fixture.root().createDirectoryAt("a", core::fs::CreationAccess::Default)).has_value());
    fixture.createFile("f", "content");
    auto const nf = SymlinkPolicy::NoFollow;

    // Each would reach `a/b` beneath the root if the kernel resolved the name.
    for (auto const* name: { "a/b", "a/." })
    {
        CAPTURE(name);
        CHECK(
            outcomeOf(fixture.run(fixture.root().createDirectoryAt(name, core::fs::CreationAccess::Default)))
            == FsErrorCode::Invalid);
        CHECK(outcomeOf(fixture.run(fixture.root().statAt(nf, name))) == FsErrorCode::Invalid);
        CHECK(
            outcomeOf(fixture.run(fixture.root().openAt(nf, name, OpenFlags::Create, DescriptorFlags::Write)))
            == FsErrorCode::Invalid);
        CHECK(outcomeOf(fixture.run(fixture.root().renameAt("f", &fixture.root(), name)))
              == FsErrorCode::Invalid);
    }
    CHECK(outcomeOf(fixture.run(fixture.root().removeDirectoryAt("a/."))) == FsErrorCode::Invalid);
    CHECK(fixture.run(fixture.root().statAt(nf, "f")).has_value());
#ifdef _WIN32
    // Windows splits on `\` too, and `:` names a stream.
    for (auto const* name: { "a\\b", "f:stream" })
    {
        CAPTURE(name);
        CHECK(
            outcomeOf(fixture.run(fixture.root().openAt(nf, name, OpenFlags::Create, DescriptorFlags::Write)))
            == FsErrorCode::Invalid);
    }
#endif
}

TEST_CASE("native: a chain of links longer than maxSymlinkHops is Loop", "[fs][native]")
{
    auto fixture = DescriptorFixture::createNative({ .maxSymlinkHops = 2 });
    if (fixture.requireSymlinks() == SymlinkSupport::Unavailable)
        SKIP("this file system cannot create symbolic links");
    fixture.createFile("f", "content");
    REQUIRE(fixture.run(fixture.root().symlinkAt("f", "one")).has_value());
    REQUIRE(fixture.run(fixture.root().symlinkAt("one", "two")).has_value());
    REQUIRE(fixture.run(fixture.root().symlinkAt("two", "three")).has_value());
    auto const follow = SymlinkPolicy::Follow;

    CHECK(fixture.run(fixture.root().statAt(follow, "two"))->size == 7);
    CHECK(outcomeOf(fixture.run(fixture.root().statAt(follow, "three"))) == FsErrorCode::Loop);
    CHECK(
        outcomeOf(fixture.run(fixture.root().openAt(follow, "three", OpenFlags::None, DescriptorFlags::Read)))
        == FsErrorCode::Loop);
}

TEST_CASE("native: a file system is not built to read nothing", "[fs][native]")
{
    auto home = core::async::testing::ManualExecutor {};
    auto blocking = core::async::testing::ManualExecutor {};
    auto const clock = core::platform::ManualWallClock {};
    auto random = core::fs::test::FixedRandom { 1 };
    auto const made =
        core::fs::makeNativeDescriptorFileSystem(home, blocking, clock, random, { .maxReadChunk = 0 });
    REQUIRE_FALSE(made.has_value());
    CHECK(made.error().code == FsErrorCode::Invalid);
}
