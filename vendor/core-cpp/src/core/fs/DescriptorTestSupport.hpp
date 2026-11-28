// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What the fs cases share: a fixture that owns a file system of either kind together with the two
/// executors, the wall clock and the random source it runs on, and drives a descriptor operation to
/// its end; its root is a directory beneath an outside one, where escape cases plant decoys, and a
/// swapper thread races a directory against a link for the escape cases. Test-only: in no
/// `FILE_SET`, so never installed.

#include <core/Random.hpp>
#include <core/async/Task.hpp>
#include <core/async/testing/ManualExecutor.hpp>
#include <core/fs/Descriptor.hpp>
#include <core/fs/FsError.hpp>
#include <core/fs/NativeFileSystem.hpp>
#include <core/fs/PathPolicy.hpp>
#include <core/fs/testing/InMemoryDescriptorFileSystem.hpp>
#include <core/platform/Clock.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace core::fs::test
{

/// Whether this process runs under wine rather than on Windows or a POSIX system. Defined per
/// platform (`posix/DescriptorTestSupport.cpp`, `windows/DescriptorTestSupport.cpp`); only a binary
/// that links one of them may call it. A case uses it to accept a behaviour wine has and no native
/// file system does, and to fail where the same behaviour shows natively. A @c bool because the
/// name asks it.
[[nodiscard]] bool runsUnderWine() noexcept;

/// Which implementation of @c IDescriptorFileSystem a fixture builds.
enum class FixtureKind : std::uint8_t
{
    InMemory, ///< @c testing::InMemoryDescriptorFileSystem.
    Native,   ///< The operating system's, over a temporary directory.
};

/// Whether the file system under a fixture can create symbolic links.
enum class SymlinkSupport : std::uint8_t
{
    Unavailable, ///< It cannot; a case that needs one skips.
    Available,   ///< It can.
};

/// What a file system sets a time to for @c SetToNow.
enum class NowSource : std::uint8_t
{
    InjectedClock, ///< The wall clock it was built with (in memory; native on Windows).
    SystemClock,   ///< The kernel's own clock (`UTIME_NOW`, native on POSIX).
};

/// What a symbolic link that names a directory is to the native file system, beside being a link.
enum class DirectoryLinkNature : std::uint8_t
{
    File,      ///< A file like any other: hard linked, and replaced by a rename (POSIX).
    Directory, ///< A directory: never hard linked, and replaced by no file (Windows).
};

/// What a directory link is to the native file system of this system.
#ifdef _WIN32
inline constexpr auto NativeDirectoryLinkNature = DirectoryLinkNature::Directory;
#else
inline constexpr auto NativeDirectoryLinkNature = DirectoryLinkNature::File;
#endif

/// What a call came to: nothing where it ran, the code where it was refused.
using Outcome = std::optional<FsErrorCode>;

/// @param result A call's result.
/// @return Its @c Outcome.
template <typename T>
[[nodiscard]] Outcome outcomeOf(FsResult<T> const& result)
{
    if (result)
        return std::nullopt;
    return result.error().code;
}

/// @param when An instant.
/// @return The same instant as a @c FileTime.
[[nodiscard]] inline FileTime fileTimeOf(std::chrono::system_clock::time_point when)
{
    auto const sinceEpoch = when.time_since_epoch();
    auto const seconds = std::chrono::floor<std::chrono::seconds>(sinceEpoch);
    return FileTime {
        .seconds = seconds.count(),
        .nanoseconds = static_cast<std::uint32_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(sinceEpoch - seconds).count())
    };
}

/// @param text Characters.
/// @return The same characters as bytes.
[[nodiscard]] inline std::vector<std::byte> bytesOf(std::string_view text)
{
    auto bytes = std::vector<std::byte>(text.size());
    std::ranges::transform(text, bytes.begin(), [](char c) { return static_cast<std::byte>(c); });
    return bytes;
}

/// @param bytes Bytes.
/// @return The same bytes as characters.
[[nodiscard]] inline std::string textOf(std::span<std::byte const> bytes)
{
    auto text = std::string(bytes.size(), '\0');
    std::ranges::transform(bytes, text.begin(), [](std::byte b) { return static_cast<char>(b); });
    return text;
}

/// An @c IRandom that fills from a seed, so two instances with one seed agree and two with
/// different seeds do not.
class FixedRandom final: public IRandom
{
  public:
    /// @param seed Where the sequence starts.
    explicit FixedRandom(std::uint64_t seed) noexcept: _state { seed } {}

    /// Fills @p out from a SplitMix64 sequence.
    /// @param out Where the bytes go.
    void fill(std::span<std::byte> out) override
    {
        for (auto& byte: out)
        {
            _state += 0x9E37'79B9'7F4A'7C15U;
            auto mixed = _state;
            mixed = (mixed ^ (mixed >> 30U)) * 0xBF58'476D'1CE4'E5B9U;
            mixed = (mixed ^ (mixed >> 27U)) * 0x94D0'49BB'1331'11EBU;
            byte = static_cast<std::byte>(mixed ^ (mixed >> 31U));
        }
    }

  private:
    std::uint64_t _state;
};

/// A thread that keeps swapping a directory for a symbolic link under one name, through renames in
/// the host's file system, until it is destroyed: what a walk racing an attacker meets. Test-only,
/// so it may own a thread.
class Swapper
{
  public:
    /// Starts swapping: @p name alternates between the directory now at @p name and a link to
    /// @p target, each parked under a name of its own while the other is in place.
    /// @param directory The host directory @p name is in.
    /// @param name The directory to swap.
    /// @param target The link's target.
    Swapper(std::filesystem::path const& directory, std::string const& name, std::string const& target):
        _state { std::make_unique<State>() }
    {
        auto const place = directory / name;
        auto const parkedDirectory = directory / (name + ".directory");
        auto const parkedLink = directory / (name + ".link");
        std::filesystem::create_directory_symlink(target, parkedLink);
        _thread = std::thread { [state = _state.get(), place, parkedDirectory, parkedLink] {
            auto error = std::error_code {};
            while (!state->stop.load())
            {
                std::filesystem::rename(place, parkedDirectory, error);
                std::filesystem::rename(parkedLink, place, error);
                state->swaps.fetch_add(1);
                std::filesystem::rename(place, parkedLink, error);
                std::filesystem::rename(parkedDirectory, place, error);
                state->swaps.fetch_add(1);
            }
        } };
    }

    Swapper(Swapper const&) = delete;
    Swapper(Swapper&&) = delete;
    Swapper& operator=(Swapper const&) = delete;
    Swapper& operator=(Swapper&&) = delete;

    /// Stops the thread, leaving the directory in place.
    ~Swapper()
    {
        _state->stop.store(true);
        _thread.join();
    }

    /// @return How many times the name changed what it names.
    [[nodiscard]] std::size_t swaps() const noexcept { return _state->swaps.load(); }

  private:
    struct State
    {
        std::atomic<bool> stop { false };
        std::atomic<std::size_t> swaps { 0 };
    };

    std::unique_ptr<State> _state;
    std::thread _thread;
};

/// A file system of one kind, the executors, the clock and the random source it runs on, and its
/// root descriptor, preopened with `Read | MutateDirectory`. The root is the directory `root`
/// beneath an outside one, which an `outside` descriptor holds, so an escape case can put a decoy
/// where an escape would land and see that it was never touched.
///
/// Both executors are `ManualExecutor`s: `home` stands for the loop a caller runs on and `blocking`
/// for the pool that takes the blocking calls, so a case sees every hop as a step rather than a
/// timing. The executors are declared first and the descriptors last, so the descriptors are
/// destroyed first and the executors last; a native fixture's temporary directory is removed after
/// its root descriptor is closed.
class DescriptorFixture
{
  public:
    /// The `maxReadChunk` the file system is built with: small, so a case can exceed it cheaply.
    static constexpr std::size_t MaxReadChunk = 64;

    /// How many rounds of draining @c run makes before it calls a task stuck.
    static constexpr std::size_t MaxRounds = 10'000;

    /// The seed of the random source a native file system draws its metadata secret from, unless a
    /// case asks for another.
    static constexpr std::uint64_t DefaultSecretSeed = 1;

    /// @param kind Which file system to build: in memory under the POSIX rules, or native under the
    ///        operating system's.
    /// @return A fixture with its root descriptor open.
    /// @throws std::runtime_error Where the root cannot be opened.
    [[nodiscard]] static DescriptorFixture create(FixtureKind kind)
    {
        if (kind == FixtureKind::Native)
            return createNative();
        return create(kind, posixPathPolicy());
    }

    /// @param kind Which file system to build.
    /// @param policy The name rules it applies.
    /// @return A fixture with its root descriptor open.
    /// @throws std::runtime_error Where the root cannot be opened.
    [[nodiscard]] static DescriptorFixture create(FixtureKind kind, PathPolicy const& policy)
    {
        if (kind == FixtureKind::Native)
            return createNative({ .policy = &policy });
        return createInMemory(policy);
    }

    /// An in-memory file system under @p policy.
    /// @param policy The name rules it applies.
    /// @param identities Whether its `relation` can decide.
    /// @param maxAncestorDepth How many parents its `relation` walks before `Loop`.
    /// @return A fixture with its root descriptor open.
    /// @throws std::runtime_error Where the root cannot be opened.
    [[nodiscard]] static DescriptorFixture createInMemory(
        PathPolicy const& policy = posixPathPolicy(),
        testing::IdentityModel identities = testing::IdentityModel::Stable,
        std::size_t maxAncestorDepth = DefaultMaxAncestorDepth)
    {
        auto fixture = DescriptorFixture { FixtureKind::InMemory, DefaultSecretSeed };
        fixture._outsideDirectory = std::filesystem::path { "/" };
        fixture._hostDirectory = std::filesystem::path { "/root" };
        auto fileSystem = std::make_unique<testing::InMemoryDescriptorFileSystem>(*fixture._home,
                                                                                  *fixture._blocking,
                                                                                  *fixture._clock,
                                                                                  policy,
                                                                                  MaxReadChunk,
                                                                                  identities,
                                                                                  maxAncestorDepth);
        fileSystem->addDirectory("root");
        fixture._inMemory = fileSystem.get();
        fixture._fileSystem = std::move(fileSystem);
        fixture.openRootDescriptor();
        return fixture;
    }

    /// @param kind Which file system to build.
    /// @param options What a native one is built with; an in-memory one takes only its
    ///        `maxAncestorDepth`.
    /// @return A fixture with its root descriptor open.
    /// @throws std::runtime_error Where the root cannot be opened.
    [[nodiscard]] static DescriptorFixture create(FixtureKind kind, NativeFileSystemOptions options)
    {
        if (kind == FixtureKind::Native)
            return createNative(options);
        return createInMemory(posixPathPolicy(), testing::IdentityModel::Stable, options.maxAncestorDepth);
    }

    /// A native file system over a temporary directory of its own.
    /// @param options What it is built with; its `maxReadChunk` is always @c MaxReadChunk.
    /// @param secretSeed The seed of the random source its metadata secret is drawn from.
    /// @return A fixture with its root descriptor open.
    /// @throws std::runtime_error Where the root cannot be opened.
    [[nodiscard]] static DescriptorFixture createNative(NativeFileSystemOptions options = {},
                                                        std::uint64_t secretSeed = DefaultSecretSeed)
    {
        auto fixture = DescriptorFixture { FixtureKind::Native, secretSeed };
        fixture._directory = std::make_shared<core::testing::ScopedTempDir>("core-cpp-fs");
        fixture.buildNative(options);
        return fixture;
    }

    /// A second native file system over this fixture's directory, with executors, a clock and a
    /// random source of its own.
    /// @param secretSeed The seed its metadata secret is drawn from.
    /// @return The fixture; the directory lives as long as the last fixture over it.
    /// @throws std::logic_error For an in-memory fixture, whose nodes no second model can see.
    [[nodiscard]] DescriptorFixture sibling(std::uint64_t secretSeed) const
    {
        if (_kind != FixtureKind::Native)
            throw std::logic_error { "DescriptorFixture::sibling: only a native fixture has one" };
        auto fixture = DescriptorFixture { FixtureKind::Native, secretSeed };
        fixture._directory = _directory;
        fixture.buildNative(_options);
        return fixture;
    }

    /// @return Which file system this is.
    [[nodiscard]] FixtureKind kind() const noexcept { return _kind; }

    /// @return The root descriptor, `Read | MutateDirectory`.
    [[nodiscard]] IDescriptor& root() const noexcept { return *_root; }

    /// @return The file system.
    [[nodiscard]] IDescriptorFileSystem& fileSystem() const noexcept { return *_fileSystem; }

    /// @return The model under an in-memory fixture, for a setup no descriptor can make.
    /// @throws std::logic_error For a native fixture.
    [[nodiscard]] testing::InMemoryDescriptorFileSystem& inMemory() const
    {
        if (_inMemory == nullptr)
            throw std::logic_error { "DescriptorFixture::inMemory: only an in-memory fixture has a model" };
        return *_inMemory;
    }

    /// @return The directory the root is preopened on, in the host's terms.
    [[nodiscard]] std::filesystem::path const& hostDirectory() const noexcept { return _hostDirectory; }

    /// @return The directory that holds the root, in the host's terms.
    [[nodiscard]] std::filesystem::path const& outsideDirectory() const noexcept { return _outsideDirectory; }

    /// @return A descriptor of the directory that holds the root, `Read | MutateDirectory`.
    [[nodiscard]] IDescriptor& outside() const noexcept { return *_outside; }

    /// @return The wall clock the file system reads.
    [[nodiscard]] platform::ManualWallClock& clock() const noexcept { return *_clock; }

    /// @return The executor every operation resumes on.
    [[nodiscard]] async::testing::ManualExecutor& home() const noexcept { return *_home; }

    /// @return The executor every blocking call runs on.
    [[nodiscard]] async::testing::ManualExecutor& blocking() const noexcept { return *_blocking; }

    /// @return What this file system sets a time to for @c SetToNow.
    [[nodiscard]] NowSource nowSource() const noexcept
    {
        if (_kind == FixtureKind::InMemory)
            return NowSource::InjectedClock;
#ifdef _WIN32
        return NowSource::InjectedClock;
#else
        return NowSource::SystemClock;
#endif
    }

    /// Opens the root directory again, with @p flags.
    /// @param flags The rights the new descriptor carries.
    /// @return The descriptor, or what refused it.
    [[nodiscard]] FsResult<std::unique_ptr<IDescriptor>> openRoot(DescriptorFlags flags) const
    {
        return run(_fileSystem->openPreopen(_hostDirectory, flags));
    }

    /// @return Whether this file system can create symbolic links: always in memory; natively where
    ///         a probe link could be created when the fixture was built.
    [[nodiscard]] SymlinkSupport requireSymlinks() const noexcept { return _symlinks; }

    /// Creates the file @p path beneath the root holding @p text.
    /// @param path Where, relative to the root.
    /// @param text What it holds.
    /// @throws std::runtime_error Where it could not.
    void createFile(std::string path, std::string_view text) const
    {
        auto file = run(root().openAt(SymlinkPolicy::NoFollow,
                                      std::move(path),
                                      OpenFlags::Create | OpenFlags::Exclusive,
                                      DescriptorFlags::Write));
        if (!file)
            throw std::runtime_error { "DescriptorFixture::createFile: " + file.error().toString() };
        auto written = run((*file)->write(bytesOf(text), 0));
        if (!written || *written != text.size())
            throw std::runtime_error { "DescriptorFixture::createFile: the write did not complete" };
    }

    /// Creates the file @p path beneath the root holding @p text, and every directory above it.
    /// @param path Where, relative to the root, `/`-separated.
    /// @param text What it holds.
    /// @throws std::runtime_error Where it could not.
    void writeFile(std::string const& path, std::string_view text) const
    {
        auto names = std::vector<std::string> {};
        for (auto const piece: path | std::views::split('/'))
            names.emplace_back(std::string_view { piece });
        auto parent = std::string {};
        for (auto const& name: names | std::views::take(names.size() - 1))
        {
            parent += parent.empty() ? name : "/" + name;
            if (auto made = run(root().createDirectoryAt(parent, CreationAccess::Default));
                !made && made.error().code != FsErrorCode::Exist)
                throw std::runtime_error { "DescriptorFixture::writeFile: " + made.error().toString() };
        }
        createFile(path, text);
    }

    /// Creates the file @p name beside the root, holding @p text: a decoy an escape would reach.
    /// @param name Its name.
    /// @param text What it holds.
    /// @throws std::runtime_error Where it could not.
    void writeOutsideFile(std::string const& name, std::string_view text)
    {
        auto file = run(outside().openAt(
            SymlinkPolicy::NoFollow, name, OpenFlags::Create | OpenFlags::Exclusive, DescriptorFlags::Write));
        if (!file || !run((*file)->write(bytesOf(text), 0)))
            throw std::runtime_error { "DescriptorFixture::writeOutsideFile: " + name };
        file->reset();
        _decoys.push_back(name);
        recordOutside();
    }

    /// Creates the directory @p name beside the root: a decoy an escape would reach.
    /// @param name Its name.
    /// @throws std::runtime_error Where it could not.
    void createOutsideDirectory(std::string const& name)
    {
        if (auto made = run(outside().createDirectoryAt(name, CreationAccess::Default)); !made)
            throw std::runtime_error { "DescriptorFixture::createOutsideDirectory: "
                                       + made.error().toString() };
        _decoys.push_back(name);
        recordOutside();
    }

    /// @return Whether everything beside the root is as it was when the last decoy was made: the
    ///         outside directory's own times and link count, and each decoy's type, size, link
    ///         count and times. The times of an access are not compared.
    [[nodiscard]] bool outsideUnchanged() const { return snapshotOutside() == _outsideSnapshot; }

    /// @return The target an absolute link to the outside directory has, in this file system's
    ///         terms: what no `symlinkAt` may create and the host can.
    [[nodiscard]] std::string outsideAbsoluteTarget() const { return _outsideDirectory.generic_string(); }

    /// Creates a symbolic link beneath the root through the host, with a target no `symlinkAt`
    /// would accept (an absolute one).
    /// @param path Where, relative to the root.
    /// @param target What it points to, verbatim.
    void hostSymlink(std::string const& path, std::string const& target) const
    {
        if (_inMemory != nullptr)
            _inMemory->addSymlink("root/" + path, target);
        else
            std::filesystem::create_directory_symlink(target, _hostDirectory / path);
    }

    /// @param descriptor A descriptor opened through this fixture.
    /// @return Whether it is open on something other than each decoy beside the root, as their
    ///         metadata hashes tell.
    [[nodiscard]] bool isInsideRoot(IDescriptor& descriptor) const
    {
        auto const hash = run(descriptor.metadataHash());
        if (!hash)
            throw std::runtime_error { "DescriptorFixture::isInsideRoot: " + hash.error().toString() };
        return std::ranges::none_of(_decoys, [&](std::string const& name) {
            auto const decoy = run(outside().metadataHashAt(SymlinkPolicy::NoFollow, name));
            return decoy && *decoy == *hash;
        });
    }

    /// Starts a thread that swaps the directory @p name beneath the root for a link to @p target,
    /// and back, until the returned object is destroyed.
    /// @param name The directory, which must exist.
    /// @param target The link's target, relative to the root.
    /// @return The swapper.
    /// @throws std::logic_error For an in-memory fixture, whose nodes no host rename reaches.
    [[nodiscard]] std::unique_ptr<Swapper> startSwapper(std::string const& name,
                                                        std::string const& target) const
    {
        if (_kind != FixtureKind::Native)
            throw std::logic_error { "DescriptorFixture::startSwapper: only a native fixture has one" };
        return std::make_unique<Swapper>(_hostDirectory, name, target);
    }

    /// Drives @p task to its end: resumes it, then drains `home` and `blocking` in turn.
    /// @param task A task over this fixture's file system.
    /// @return Its result.
    /// @throws std::runtime_error Where it is still suspended after @c MaxRounds rounds, or with
    ///         nothing queued on either executor; its frame is then leaked rather than freed while
    ///         something may still point into it.
    template <typename T>
    [[nodiscard]] T run(async::Task<T> task) const
    {
        task.handle().resume();
        for ([[maybe_unused]] auto const round: std::views::iota(std::size_t { 0 }, MaxRounds))
        {
            if (task.done() || _home->drain() + _blocking->drain() == 0)
                break;
        }
        if (task.done())
            return task.result();
        auto const homePending = _home->pending();
        auto const blockingPending = _blocking->pending();
        std::ignore = task.release();
        throw std::runtime_error { "DescriptorFixture::run: the task is still suspended (home pending "
                                   + std::to_string(homePending) + ", blocking pending "
                                   + std::to_string(blockingPending)
                                   + "): waited for it to complete on the two executors" };
    }

    /// Reads @p stream to its end, each answer as it came: an entry, or why that entry is not one.
    /// @param stream A stream over this fixture's file system.
    /// @param maxAnswers How many answers the directory can hold at most; one more is a stream that
    ///        does not end.
    /// @return The answers, in order.
    /// @throws std::runtime_error Where the stream has not ended after @p maxAnswers answers.
    [[nodiscard]] std::vector<FsResult<DirectoryEntry>> readEntries(IDirectoryStream& stream,
                                                                    std::size_t maxAnswers) const
    {
        auto answers = std::vector<FsResult<DirectoryEntry>> {};
        while (answers.size() <= maxAnswers)
        {
            auto next = run(stream.next());
            if (next && !*next)
                return answers;
            if (next)
                answers.emplace_back(std::move(**next));
            else
                answers.emplace_back(std::unexpected { std::move(next.error()) });
        }
        throw std::runtime_error { "DescriptorFixture::readEntries: the stream had not ended after "
                                   + std::to_string(maxAnswers) + " answers" };
    }

  private:
    DescriptorFixture(FixtureKind kind, std::uint64_t secretSeed):
        _random { std::make_unique<FixedRandom>(secretSeed) }, _kind { kind }
    {
    }

    void buildNative(NativeFileSystemOptions options)
    {
        options.maxReadChunk = MaxReadChunk;
        _options = options;
        _outsideDirectory = _directory->path();
        _hostDirectory = _outsideDirectory / "root";
        std::filesystem::create_directories(_hostDirectory);
        auto fileSystem = makeNativeDescriptorFileSystem(*_home, *_blocking, *_clock, *_random, options);
        if (!fileSystem)
            throw std::runtime_error { "DescriptorFixture: no native file system: "
                                       + fileSystem.error().toString() };
        _fileSystem = std::move(*fileSystem);
        openRootDescriptor();
        _symlinks = probeSymlinks();
    }

    void openRootDescriptor()
    {
        auto root = openRoot(DescriptorFlags::Read | DescriptorFlags::MutateDirectory);
        if (!root)
            throw std::runtime_error { "DescriptorFixture: the root did not open: "
                                       + root.error().toString() };
        _root = std::move(*root);
        auto outside = run(_fileSystem->openPreopen(
            _outsideDirectory, DescriptorFlags::Read | DescriptorFlags::MutateDirectory));
        if (!outside)
            throw std::runtime_error { "DescriptorFixture: the outside directory did not open: "
                                       + outside.error().toString() };
        _outside = std::move(*outside);
        recordOutside();
    }

    /// What @c outsideUnchanged compares: a stat without its access time.
    struct Seen
    {
        DescriptorType type = DescriptorType::Unknown;
        std::uint64_t linkCount = 0;
        std::uint64_t size = 0;
        std::optional<FileTime> modified;
        std::optional<FileTime> statusChanged;

        bool operator==(Seen const&) const = default;
    };

    [[nodiscard]] static std::optional<Seen> seenOf(FsResult<DescriptorStat> const& status)
    {
        if (!status)
            return std::nullopt;
        return Seen { .type = status->type,
                      .linkCount = status->linkCount,
                      .size = status->size,
                      .modified = status->modified,
                      .statusChanged = status->statusChanged };
    }

    [[nodiscard]] std::vector<std::optional<Seen>> snapshotOutside() const
    {
        auto seen = std::vector<std::optional<Seen>> { seenOf(run(outside().stat())) };
        for (auto const& name: _decoys)
            seen.push_back(seenOf(run(outside().statAt(SymlinkPolicy::NoFollow, name))));
        return seen;
    }

    void recordOutside() { _outsideSnapshot = snapshotOutside(); }

    /// Creates and removes a link, so a case that needs one skips where the system refuses it (a
    /// Windows account without the symbolic-link privilege).
    [[nodiscard]] SymlinkSupport probeSymlinks() const
    {
        constexpr auto ProbeName = std::string_view { ".core-cpp-symlink-probe" };
        if (!run(root().symlinkAt("probe-target", std::string { ProbeName })))
            return SymlinkSupport::Unavailable;
        if (auto removed = run(root().unlinkFileAt(std::string { ProbeName })); !removed)
            throw std::runtime_error { "DescriptorFixture: the symlink probe was not removed: "
                                       + removed.error().toString() };
        return SymlinkSupport::Available;
    }

    std::unique_ptr<async::testing::ManualExecutor> _home =
        std::make_unique<async::testing::ManualExecutor>();
    std::unique_ptr<async::testing::ManualExecutor> _blocking =
        std::make_unique<async::testing::ManualExecutor>();
    std::unique_ptr<platform::ManualWallClock> _clock = std::make_unique<platform::ManualWallClock>(
        std::chrono::system_clock::time_point { std::chrono::seconds { 1'700'000'000 } });
    std::unique_ptr<FixedRandom> _random;
    std::shared_ptr<core::testing::ScopedTempDir> _directory;
    std::filesystem::path _outsideDirectory;
    std::filesystem::path _hostDirectory;
    NativeFileSystemOptions _options;
    std::unique_ptr<IDescriptorFileSystem> _fileSystem;
    testing::InMemoryDescriptorFileSystem* _inMemory = nullptr; ///< @c _fileSystem, where in memory.
    std::unique_ptr<IDescriptor> _root;
    std::unique_ptr<IDescriptor> _outside;
    std::vector<std::string> _decoys;
    std::vector<std::optional<Seen>> _outsideSnapshot;
    FixtureKind _kind;
    SymlinkSupport _symlinks = SymlinkSupport::Available;
};

} // namespace core::fs::test
