// SPDX-License-Identifier: Apache-2.0
#include <core/fs/testing/InMemoryDescriptorFileSystem.hpp>

#include <core/async/IExecutor.hpp>
#include <core/async/Offload.hpp>
#include <core/async/Task.hpp>
#include <core/fs/AccessScope.hpp>
#include <core/fs/Descriptor.hpp>
#include <core/fs/FsError.hpp>
#include <core/fs/NativeFileSystem.hpp>
#include <core/fs/PathPolicy.hpp>
#include <core/fs/Rights.hpp>
#include <core/fs/detail/Ancestry.hpp>
#include <core/fs/detail/DirectoryStream.hpp>
#include <core/fs/detail/PathWalk.hpp>
#include <core/platform/Clock.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace core::fs::testing
{

namespace
{

    /// The largest file the model holds: a write or a size beyond it is `FileTooLarge`, so a guest
    /// cannot make the host allocate what it asks for.
    constexpr std::uint64_t MaxFileSize = std::uint64_t { 1 } << 30U;

    /// The inode number of the model's root directory.
    constexpr std::uint64_t RootInode = 1;

    std::unexpected<FsError> refuse(FsErrorCode code)
    {
        return std::unexpected { FsError { .code = code, .systemCode = 0, .context = {} } };
    }

} // namespace

/// The model, and what its descriptors need to reach it. Every member function expects @c mutex held.
struct InMemoryState
{
    // The model's types are nested, so they share the linkage of the struct the header declares.

    /// A name in a directory, as given, and what it names.
    struct Entry
    {
        std::string name;
        std::uint64_t inode = 0;
    };

    /// A directory, a regular file or a symbolic link. Only the members of its type are used.
    struct Node
    {
        std::vector<std::byte> data;          ///< A file's bytes.
        std::map<std::string, Entry> entries; ///< A directory's entries, keyed by folded name.
        std::string target;                   ///< A link's target.
        std::uint64_t linkCount = 1;
        std::uint64_t changes = 0; ///< Bumped by every change, for the metadata hash.
        FileTime accessed;
        FileTime modified;
        FileTime statusChanged;
        DescriptorType type = DescriptorType::Unknown;
        AccessFacts access; ///< Who may reach it, as a native reader would have found it.
        /// A directory's real parent: the root's is itself, and a directory removed or replaced has
        /// none (0), as a native `..` of it finds nothing.
        std::uint64_t parent = 0;
    };

    /// Where a path resolved to: the directories walked from the descriptor down to the one holding
    /// the final name, the name, what it names, if anything, and whether the path required that to
    /// be a directory (`name/`). An empty name means the path named the last directory itself (`.`,
    /// `a/..`).
    struct Location
    {
        std::vector<std::uint64_t> directories;
        std::string name;
        std::optional<std::uint64_t> node;
        fs::detail::FinalComponent finalComponent = fs::detail::FinalComponent::Any;

        [[nodiscard]] std::uint64_t parent() const { return directories.back(); }
    };

    using TrailingSlashOnLink = fs::detail::TrailingSlashOnLink;

    /// The directories a walk of the model moves through, by inode: what @c fs::detail::walkBeneath
    /// walks, so the model and the native descriptors share every rule of resolution.
    class Cursor final: public fs::detail::IBeneathCursor
    {
      public:
        Cursor(InMemoryState const& state, std::uint64_t start): _state { &state }, _directories { start } {}

        [[nodiscard]] bool atRoot() const noexcept override { return _directories.size() == 1; }

        [[nodiscard]] FsResult<fs::detail::EntryKind> enter(std::string const& name) override
        {
            auto const found = _state->lookup(_directories.back(), name);
            if (!found)
                return refuse(FsErrorCode::NoEntry);
            auto const type = _state->nodes.at(*found).type;
            if (type == DescriptorType::SymbolicLink)
                return fs::detail::EntryKind::SymbolicLink;
            if (type != DescriptorType::Directory)
                return refuse(FsErrorCode::NotDirectory);
            _directories.push_back(*found);
            return fs::detail::EntryKind::Directory;
        }

        void leave() override { _directories.pop_back(); }

        [[nodiscard]] FsResult<fs::detail::EntryKind> inspect(std::string const& name) override
        {
            auto const found = _state->lookup(_directories.back(), name);
            if (!found)
                return fs::detail::EntryKind::Missing;
            auto const type = _state->nodes.at(*found).type;
            if (type == DescriptorType::Directory)
                return fs::detail::EntryKind::Directory;
            if (type == DescriptorType::SymbolicLink)
                return fs::detail::EntryKind::SymbolicLink;
            return fs::detail::EntryKind::Other;
        }

        [[nodiscard]] FsResult<std::string> readLink(std::string const& name) override
        {
            auto const found = _state->lookup(_directories.back(), name);
            if (!found || _state->nodes.at(*found).type != DescriptorType::SymbolicLink)
                return refuse(FsErrorCode::Invalid);
            return _state->nodes.at(*found).target;
        }

        [[nodiscard]] FsResult<fs::detail::Shortcut> shortcut(std::span<std::string const>) override
        {
            return fs::detail::Shortcut::Declined;
        }

        void restart() override { _directories.resize(1); }

        /// @return The directories entered, from the start down to the current one.
        [[nodiscard]] std::vector<std::uint64_t> const& directories() const noexcept { return _directories; }

      private:
        InMemoryState const* _state;
        std::vector<std::uint64_t> _directories;
    };

    async::IExecutor* home;
    async::IExecutor* blocking;
    platform::IWallClock const* clock;
    PathPolicy const* policy;
    std::size_t maxReadChunk;
    IdentityModel identities;
    std::size_t maxAncestorDepth;
    std::mutex mutex;
    std::map<std::uint64_t, Node> nodes;
    std::uint64_t nextInode = RootInode;

    InMemoryState(async::IExecutor& homeExecutor,
                  async::IExecutor& blockingExecutor,
                  platform::IWallClock const& wallClock,
                  PathPolicy const& pathPolicy,
                  std::size_t readChunk,
                  IdentityModel identityModel,
                  std::size_t ancestorDepth):
        home { &homeExecutor },
        blocking { &blockingExecutor },
        clock { &wallClock },
        policy { &pathPolicy },
        maxReadChunk { readChunk },
        identities { identityModel },
        maxAncestorDepth { ancestorDepth }
    {
        nodes.at(createNode(DescriptorType::Directory)).parent = RootInode;
    }

    [[nodiscard]] FileTime now() const
    {
        auto const sinceEpoch = clock->now().time_since_epoch();
        auto const seconds = std::chrono::floor<std::chrono::seconds>(sinceEpoch);
        return FileTime {
            .seconds = seconds.count(),
            .nanoseconds = static_cast<std::uint32_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(sinceEpoch - seconds).count()),
        };
    }

    void touch(Node& node, FileTime when)
    {
        node.modified = when;
        node.statusChanged = when;
        ++node.changes;
    }

    /// The mode a new node gets: a umask of 022 for @c CreationAccess::Default, as most systems have.
    [[nodiscard]] static std::uint32_t modeOf(DescriptorType type, CreationAccess access) noexcept
    {
        if (type == DescriptorType::SymbolicLink)
            return 0777;
        auto const searchable = type == DescriptorType::Directory;
        if (access == CreationAccess::OwnerOnly)
            return searchable ? 0700 : 0600;
        return searchable ? 0755 : 0644;
    }

    [[nodiscard]] std::uint64_t createNode(DescriptorType type,
                                           CreationAccess access = CreationAccess::Default)
    {
        auto const inode = nextInode++;
        auto const when = now();
        nodes.emplace(inode,
                      Node { .data = {},
                             .entries = {},
                             .target = {},
                             .linkCount = 1,
                             .changes = 0,
                             .accessed = when,
                             .modified = when,
                             .statusChanged = when,
                             .type = type,
                             .access = AccessFacts { .owner = AccessFacts::Owner::CurrentUser,
                                                     .posixMode = modeOf(type, access),
                                                     .acl = AccessFacts::AclState::Absent,
                                                     .entries = {} } });
        return inode;
    }

    /// @return @p name as a directory's key: as given, or with ASCII letters folded where the policy
    ///         says case does not distinguish names.
    [[nodiscard]] std::string keyOf(std::string_view name) const
    {
        auto key = std::string { name };
        if (policy->caseRule == CaseRule::Insensitive)
            std::ranges::transform(key, key.begin(), [](char c) {
                return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
            });
        return key;
    }

    [[nodiscard]] std::optional<std::uint64_t> lookup(std::uint64_t directory, std::string_view name) const
    {
        auto const& entries = nodes.at(directory).entries;
        auto const found = entries.find(keyOf(name));
        if (found == entries.end())
            return std::nullopt;
        return found->second.inode;
    }

    void insert(std::uint64_t directory, std::string name, std::uint64_t inode)
    {
        auto key = keyOf(name);
        detach(directory, key);
        if (auto& node = nodes.at(inode); node.type == DescriptorType::Directory)
            node.parent = directory;
        auto& parent = nodes.at(directory);
        parent.entries.insert_or_assign(std::move(key), Entry { .name = std::move(name), .inode = inode });
        touch(parent, now());
    }

    void erase(std::uint64_t directory, std::string_view name)
    {
        auto const key = keyOf(name);
        detach(directory, key);
        auto& parent = nodes.at(directory);
        parent.entries.erase(key);
        touch(parent, now());
    }

    /// Takes the parent of the directory @p key names in @p directory, which is losing that entry.
    void detach(std::uint64_t directory, std::string const& key)
    {
        auto const& entries = nodes.at(directory).entries;
        if (auto const found = entries.find(key); found != entries.end())
            if (auto& node = nodes.at(found->second.inode);
                node.type == DescriptorType::Directory && node.parent == directory)
                node.parent = 0;
    }

    /// @return @p inode's identity, then each real parent's up to the model's root; `NoEntry` past
    ///         a directory with no parent, `Loop` past @c maxAncestorDepth parents.
    [[nodiscard]] FsResult<std::vector<fs::detail::ObjectIdentity>> ancestryOf(std::uint64_t inode) const
    {
        // Grown per step, never reserved to the cap, as the native walkers do. At most
        // maxAncestorDepth + 1 parents are looked at, and the last of them must be the root.
        auto chain = std::vector { fs::detail::ObjectIdentity { .device = 0, .idLow = inode, .idHigh = 0 } };
        auto current = inode;
        for ([[maybe_unused]] auto const step: std::views::iota(std::size_t { 0 }, maxAncestorDepth + 1))
        {
            auto const parent = nodes.at(current).parent;
            if (parent == 0)
                return refuse(FsErrorCode::NoEntry);
            if (parent == current)
                return chain;
            chain.push_back(fs::detail::ObjectIdentity { .device = 0, .idLow = parent, .idHigh = 0 });
            current = parent;
        }
        return refuse(FsErrorCode::Loop);
    }

    void dropLink(std::uint64_t inode)
    {
        auto& node = nodes.at(inode);
        --node.linkCount;
        node.statusChanged = now();
    }

    /// Resolves @p path beneath @p start through @c fs::detail::walkBeneath, following the final
    /// link where @p policyForFinal, or a trailing `/` under @c TrailingSlashOnLink::Follow, says so.
    /// @return Where it led; `NotDirectory` where @p start is not a directory, and what the walk
    ///         answers.
    [[nodiscard]] FsResult<Location> walk(std::uint64_t start,
                                          std::string_view path,
                                          SymlinkPolicy policyForFinal,
                                          TrailingSlashOnLink onLink = TrailingSlashOnLink::Follow) const
    {
        if (nodes.at(start).type != DescriptorType::Directory)
            return refuse(FsErrorCode::NotDirectory);
        auto cursor = Cursor { *this, start };
        auto const rules = fs::detail::WalkRules { .policy = policy,
                                                   .finalPolicy = policyForFinal,
                                                   .onLink = onLink,
                                                   .maxSymlinkHops = DefaultMaxSymlinkHops,
                                                   .kernelSeparators = {} };
        return fs::detail::planRelativePath(path, *policy)
            .and_then([&](fs::detail::PathPlan plan) {
                return fs::detail::walkBeneath(cursor, std::move(plan), rules);
            })
            .transform([&](fs::detail::WalkEnd end) {
                auto const parent = cursor.directories().back();
                auto const node = end.name.empty() ? std::optional { parent } : lookup(parent, end.name);
                return Location { .directories = cursor.directories(),
                                  .name = std::move(end.name),
                                  .node = node,
                                  .finalComponent = end.finalComponent };
            });
    }

    /// @return What @p path names; `NoEntry` where nothing does.
    [[nodiscard]] FsResult<std::uint64_t> existing(std::uint64_t start,
                                                   std::string_view path,
                                                   SymlinkPolicy policyForFinal) const
    {
        return walk(start, path, policyForFinal)
            .and_then([](Location const& location) -> FsResult<std::uint64_t> {
                if (!location.node)
                    return refuse(FsErrorCode::NoEntry);
                return *location.node;
            });
    }

    /// @return Where @p path would be created as a @p creating; `Exist` where something is there
    ///         already, `NotDirectory` where the path ends in `/` and @p creating is not a directory.
    [[nodiscard]] FsResult<Location> vacant(std::uint64_t start,
                                            std::string_view path,
                                            DescriptorType creating) const
    {
        return walk(start, path, SymlinkPolicy::NoFollow)
            .and_then([creating](Location location) -> FsResult<Location> {
                if (location.node)
                    return refuse(FsErrorCode::Exist);
                if (creating != DescriptorType::Directory
                    && location.finalComponent == fs::detail::FinalComponent::Directory)
                    return refuse(FsErrorCode::NotDirectory);
                return location;
            });
    }

    /// Resizes a file to @p size bytes, refusing what a guest could ask for and the host cannot give.
    /// @return Nothing; `FileTooLarge` above @c MaxFileSize, `InsufficientMemory` where the
    ///         allocation fails.
    [[nodiscard]] static FsResult<void> resizeData(Node& node, std::uint64_t size)
    {
        if (size > MaxFileSize)
            return refuse(FsErrorCode::FileTooLarge);
        try
        {
            node.data.resize(static_cast<std::size_t>(size));
        }
        catch (std::bad_alloc const&)
        {
            return refuse(FsErrorCode::InsufficientMemory);
        }
        return {};
    }

    [[nodiscard]] DescriptorStat statOf(std::uint64_t inode) const
    {
        auto const& node = nodes.at(inode);
        auto size = std::uint64_t { 0 };
        if (node.type == DescriptorType::RegularFile)
            size = node.data.size();
        else if (node.type == DescriptorType::SymbolicLink)
            size = node.target.size();
        return DescriptorStat { .type = node.type,
                                .linkCount = node.linkCount,
                                .size = size,
                                .accessed = node.accessed,
                                .modified = node.modified,
                                .statusChanged = node.statusChanged,
                                .access = classifyAccess(node.access) };
    }

    void applyTimes(std::uint64_t inode, NewTimestamp const& accessed, NewTimestamp const& modified)
    {
        auto& node = nodes.at(inode);
        auto const when = now();
        auto const apply = [when](FileTime& slot, NewTimestamp const& change) {
            if (std::holds_alternative<SetToNow>(change))
                slot = when;
            else if (auto const* time = std::get_if<FileTime>(&change))
                slot = *time;
        };
        apply(node.accessed, accessed);
        apply(node.modified, modified);
        node.statusChanged = when;
        ++node.changes;
    }

    [[nodiscard]] FsResult<std::size_t> writeAt(std::uint64_t inode,
                                                std::vector<std::byte> const& bytes,
                                                std::uint64_t offset)
    {
        auto& node = nodes.at(inode);
        if (node.type == DescriptorType::Directory)
            return refuse(FsErrorCode::IsDirectory);
        if (offset > MaxFileSize || bytes.size() > MaxFileSize - offset)
            return refuse(FsErrorCode::FileTooLarge);
        auto const end = offset + bytes.size();
        if (end > node.data.size())
            if (auto resized = resizeData(node, end); !resized)
                return std::unexpected { std::move(resized.error()) };
        std::ranges::copy(bytes, node.data.begin() + static_cast<std::ptrdiff_t>(offset));
        touch(node, now());
        return bytes.size();
    }

    /// Creates a node at @p path, and every directory above it, for a test's setup.
    std::uint64_t add(std::string_view path, DescriptorType type)
    {
        auto names = std::vector<std::string_view> {};
        for (auto const piece: path | std::views::split('/'))
            if (auto const name = std::string_view { piece }; !name.empty() && name != ".")
                names.push_back(name);
        if (names.empty())
            throw std::invalid_argument { "InMemoryDescriptorFileSystem: an empty path names the root" };
        auto directory = RootInode;
        for (auto const name: names | std::views::take(names.size() - 1))
        {
            auto found = lookup(directory, name);
            if (!found)
            {
                found = createNode(DescriptorType::Directory);
                insert(directory, std::string { name }, *found);
            }
            if (nodes.at(*found).type != DescriptorType::Directory)
                throw std::invalid_argument { "InMemoryDescriptorFileSystem: '" + std::string { name }
                                              + "' is in the way of '" + std::string { path } + "'" };
            directory = *found;
        }
        if (auto const found = lookup(directory, names.back()))
        {
            if (type == DescriptorType::Directory && nodes.at(*found).type == DescriptorType::Directory)
                return *found;
            throw std::invalid_argument { "InMemoryDescriptorFileSystem: '" + std::string { path }
                                          + "' exists" };
        }
        auto const inode = createNode(type);
        insert(directory, std::string { names.back() }, inode);
        return inode;
    }
};

namespace
{

    using Location = InMemoryState::Location;
    using TrailingSlashOnLink = InMemoryState::TrailingSlashOnLink;

    /// A stream over a directory of the model: it reads the entries after the last key it handed
    /// out, a batch of @c fs::detail::DirectoryBatchEntries at a time, so a change to the directory
    /// between two batches is met as a native stream meets it: a name removed ahead is not read, a
    /// name added ahead is, and nothing is read twice.
    class InMemoryDirectoryStream final: public fs::detail::BatchedDirectoryStream
    {
      public:
        InMemoryDirectoryStream(std::shared_ptr<InMemoryState> state, std::uint64_t inode):
            _state { std::move(state) }, _inode { inode }
        {
        }

      protected:
        async::Task<FsResult<fs::detail::DirectoryBatch>> fetch() override
        {
            auto read = co_await async::offload(
                _state->home, _state->blocking, [state = _state, inode = _inode, after = _after]() {
                    auto const lock = std::scoped_lock { state->mutex };
                    return readBatch(*state, inode, after);
                });
            _after = std::move(read.last);
            co_return std::move(read.batch);
        }

      private:
        struct Read
        {
            fs::detail::DirectoryBatch batch;
            std::optional<std::string> last; ///< The key of the last entry read.
        };

        static Read readBatch(InMemoryState const& state,
                              std::uint64_t inode,
                              std::optional<std::string> const& after)
        {
            auto const& entries = state.nodes.at(inode).entries;
            auto position = after ? entries.upper_bound(*after) : entries.begin();
            auto read = Read { .batch = {}, .last = after };
            while (position != entries.end() && read.batch.entries.size() < fs::detail::DirectoryBatchEntries)
            {
                read.batch.entries.emplace_back(DirectoryEntry {
                    .type = state.nodes.at(position->second.inode).type, .name = position->second.name });
                read.last = position->first;
                ++position;
            }
            if (position == entries.end())
                read.batch.end = StreamEnd::Reached;
            return read;
        }

        std::shared_ptr<InMemoryState> _state;
        std::uint64_t _inode;
        std::optional<std::string> _after; ///< The key of the last entry handed out; none yet.
    };

    /// A descriptor of the model: the node it is open on, and the rights it was opened with.
    class InMemoryDescriptor final: public IDescriptor
    {
      private:
        // First, because a function whose return type is deduced cannot be called above its definition.

        /// Runs @p work on the blocking executor with the model locked, and resumes on home; @p work
        /// takes the model's owner, so it can hand a new descriptor its share.
        template <typename F>
        [[nodiscard]] auto submitShared(F work) const
        {
            return async::offload(
                _state->home, _state->blocking, [state = _state, work = std::move(work)]() mutable {
                    auto const lock = std::scoped_lock { state->mutex };
                    return work(state);
                });
        }

        /// @c submitShared for work that needs only the model.
        template <typename F>
        [[nodiscard]] auto submit(F work) const
        {
            return submitShared(
                [work = std::move(work)](std::shared_ptr<InMemoryState> const& state) mutable {
                    return work(*state);
                });
        }

      public:
        InMemoryDescriptor(std::shared_ptr<InMemoryState> state, std::uint64_t inode, DescriptorFlags flags):
            _state { std::move(state) }, _inode { inode }, _flags { flags }
        {
        }

        /// @param descriptor Any descriptor.
        /// @param state A model's.
        /// @return The node @p descriptor is open on where it is a descriptor of that model.
        [[nodiscard]] static std::optional<std::uint64_t> nodeOf(IDescriptor const& descriptor,
                                                                 std::shared_ptr<InMemoryState> const& state)
        {
            auto const* mine = dynamic_cast<InMemoryDescriptor const*>(&descriptor);
            if (mine == nullptr || mine->_state != state)
                return std::nullopt;
            return mine->_inode;
        }

        async::Task<FsResult<ReadChunk>> read(std::size_t maxBytes, std::uint64_t offset) override
        {
            return submit([inode = _inode, flags = _flags, maxBytes, offset](
                              InMemoryState& state) -> FsResult<ReadChunk> {
                return checkRights(flags, Operation::Read).and_then([&]() -> FsResult<ReadChunk> {
                    auto const& node = state.nodes.at(inode);
                    if (node.type == DescriptorType::Directory)
                        return refuse(FsErrorCode::IsDirectory);
                    auto const size = node.data.size();
                    auto const begin = std::min<std::uint64_t>(offset, size);
                    auto const count =
                        std::min({ maxBytes, state.maxReadChunk, static_cast<std::size_t>(size - begin) });
                    auto const first = node.data.begin() + static_cast<std::ptrdiff_t>(begin);
                    return ReadChunk {
                        .bytes = std::vector<std::byte>(first, first + static_cast<std::ptrdiff_t>(count)),
                        .end = begin + count >= size ? StreamEnd::Reached : StreamEnd::More,
                    };
                });
            });
        }

        async::Task<FsResult<std::size_t>> write(std::vector<std::byte> bytes, std::uint64_t offset) override
        {
            return submit(
                [inode = _inode, flags = _flags, bytes = std::move(bytes), offset](InMemoryState& state) {
                    return checkRights(flags, Operation::Write).and_then([&] {
                        return state.writeAt(inode, bytes, offset);
                    });
                });
        }

        async::Task<FsResult<std::size_t>> append(std::vector<std::byte> bytes) override
        {
            return submit([inode = _inode, flags = _flags, bytes = std::move(bytes)](InMemoryState& state) {
                return checkRights(flags, Operation::Append).and_then([&] {
                    return state.writeAt(inode, bytes, state.nodes.at(inode).data.size());
                });
            });
        }

        async::Task<FsResult<void>> advise(std::uint64_t, std::uint64_t, Advice) override
        {
            return submit([flags = _flags](InMemoryState&) { return checkRights(flags, Operation::Advise); });
        }

        async::Task<FsResult<void>> syncData() override
        {
            return submit([flags = _flags](InMemoryState&) { return checkRights(flags, Operation::Sync); });
        }

        async::Task<FsResult<void>> sync() override
        {
            return submit([flags = _flags](InMemoryState&) { return checkRights(flags, Operation::Sync); });
        }

        [[nodiscard]] DescriptorFlags flags() const noexcept override { return _flags; }

        async::Task<FsResult<DescriptorType>> type() override
        {
            return submit([inode = _inode, flags = _flags](InMemoryState& state) {
                return checkRights(flags, Operation::Stat).transform([&] {
                    return state.nodes.at(inode).type;
                });
            });
        }

        async::Task<FsResult<void>> setSize(std::uint64_t size) override
        {
            return submit([inode = _inode, flags = _flags, size](InMemoryState& state) {
                return checkRights(flags, Operation::SetSize).and_then([&]() -> FsResult<void> {
                    auto& node = state.nodes.at(inode);
                    if (node.type == DescriptorType::Directory)
                        return refuse(FsErrorCode::IsDirectory);
                    return InMemoryState::resizeData(node, size).transform([&] {
                        state.touch(node, state.now());
                    });
                });
            });
        }

        async::Task<FsResult<void>> setTimes(NewTimestamp accessed, NewTimestamp modified) override
        {
            return submit([inode = _inode, flags = _flags, accessed, modified](InMemoryState& state) {
                return checkRights(flags, Operation::SetTimes).transform([&] {
                    state.applyTimes(inode, accessed, modified);
                });
            });
        }

        async::Task<FsResult<DescriptorStat>> stat() override
        {
            return submit([inode = _inode, flags = _flags](InMemoryState& state) {
                return checkRights(flags, Operation::Stat).transform([&] { return state.statOf(inode); });
            });
        }

        async::Task<FsResult<MetadataHash>> metadataHash() override
        {
            return submit([inode = _inode, flags = _flags](InMemoryState& state) {
                return checkRights(flags, Operation::MetadataHash).transform([&] {
                    return MetadataHash { .lower = inode, .upper = state.nodes.at(inode).changes };
                });
            });
        }

        async::Task<FsResult<std::unique_ptr<IDirectoryStream>>> readDirectory() override
        {
            return submitShared([inode = _inode, flags = _flags](std::shared_ptr<InMemoryState> const& shared)
                                    -> FsResult<std::unique_ptr<IDirectoryStream>> {
                return checkRights(flags, Operation::ReadDirectory)
                    .and_then([&]() -> FsResult<void> {
                        if (shared->nodes.at(inode).type != DescriptorType::Directory)
                            return refuse(FsErrorCode::NotDirectory);
                        return {};
                    })
                    .transform([&]() -> std::unique_ptr<IDirectoryStream> {
                        return std::make_unique<InMemoryDirectoryStream>(shared, inode);
                    });
            });
        }

        async::Task<FsResult<std::unique_ptr<IDescriptor>>> openAt(SymlinkPolicy policy,
                                                                   std::string path,
                                                                   OpenFlags open,
                                                                   DescriptorFlags childFlags) override
        {
            return submitShared(
                [inode = _inode, flags = _flags, policy, path = std::move(path), open, childFlags](
                    std::shared_ptr<InMemoryState> const& shared) -> FsResult<std::unique_ptr<IDescriptor>> {
                    auto& state = *shared;
                    auto const opened =
                        checkRights(flags, Operation::OpenAt)
                            .and_then([&] { return checkOpenRights(flags, open, childFlags); })
                            .and_then([&] { return state.walk(inode, path, finalPolicy(policy, open)); })
                            .and_then([&](Location const& location) {
                                return openLocation(state, location, open, childFlags);
                            });
                    if (!opened)
                        return std::unexpected { opened.error() };
                    return std::make_unique<InMemoryDescriptor>(
                        shared, *opened, grantedFlags(state.nodes.at(*opened).type, childFlags));
                });
        }

        async::Task<FsResult<void>> createDirectoryAt(std::string path, CreationAccess access) override
        {
            return submit(
                [inode = _inode, flags = _flags, path = std::move(path), access](InMemoryState& state) {
                    return checkRights(flags, Operation::CreateDirectoryAt)
                        .and_then([&] { return state.vacant(inode, path, DescriptorType::Directory); })
                        .transform([&](Location const& location) {
                            state.insert(location.parent(),
                                         location.name,
                                         state.createNode(DescriptorType::Directory, access));
                        });
                });
        }

        async::Task<FsResult<DescriptorStat>> statAt(SymlinkPolicy policy, std::string path) override
        {
            return submit(
                [inode = _inode, flags = _flags, policy, path = std::move(path)](InMemoryState& state) {
                    return checkRights(flags, Operation::StatAt)
                        .and_then([&] { return state.existing(inode, path, policy); })
                        .transform([&](std::uint64_t found) { return state.statOf(found); });
                });
        }

        async::Task<FsResult<void>> setTimesAt(SymlinkPolicy policy,
                                               std::string path,
                                               NewTimestamp accessed,
                                               NewTimestamp modified) override
        {
            return submit(
                [inode = _inode, flags = _flags, policy, path = std::move(path), accessed, modified](
                    InMemoryState& state) {
                    return checkRights(flags, Operation::SetTimesAt)
                        .and_then([&] { return state.existing(inode, path, policy); })
                        .transform([&](std::uint64_t found) { state.applyTimes(found, accessed, modified); });
                });
        }

        async::Task<FsResult<void>> linkAt(SymlinkPolicy policy,
                                           std::string oldPath,
                                           IDescriptor* newDirectory,
                                           std::string newPath) override
        {
            auto const target = sameSystemTarget(newDirectory);
            return submit([inode = _inode,
                           flags = _flags,
                           policy,
                           oldPath = std::move(oldPath),
                           target,
                           newPath = std::move(newPath)](InMemoryState& state) -> FsResult<void> {
                if (auto checked = checkRights(flags, Operation::LinkAt); !checked)
                    return checked;
                if (policy == SymlinkPolicy::Follow)
                    return refuse(FsErrorCode::Invalid);
                if (!target)
                    return std::unexpected { target.error() };
                if (auto checked = checkRights(target->flags, Operation::LinkAt); !checked)
                    return checked;
                return state.existing(inode, oldPath, SymlinkPolicy::NoFollow)
                    .and_then([&](std::uint64_t found) -> FsResult<void> {
                        if (state.nodes.at(found).type == DescriptorType::Directory)
                            return refuse(FsErrorCode::NotPermitted);
                        return state.vacant(target->inode, newPath, DescriptorType::RegularFile)
                            .transform([&](Location const& location) {
                                state.insert(location.parent(), location.name, found);
                                ++state.nodes.at(found).linkCount;
                            });
                    });
            });
        }

        async::Task<FsResult<std::string>> readlinkAt(std::string path) override
        {
            return submit([inode = _inode, flags = _flags, path = std::move(path)](InMemoryState& state) {
                return checkRights(flags, Operation::ReadlinkAt)
                    .and_then([&] { return state.existing(inode, path, SymlinkPolicy::NoFollow); })
                    .and_then([&](std::uint64_t found) -> FsResult<std::string> {
                        auto const& node = state.nodes.at(found);
                        if (node.type != DescriptorType::SymbolicLink)
                            return refuse(FsErrorCode::Invalid);
                        // One rule with the walk and symlinkAt: a rooted target is never handed out.
                        return fs::detail::refuseRootedTarget(node.target, *state.policy);
                    });
            });
        }

        async::Task<FsResult<void>> removeDirectoryAt(std::string path) override
        {
            return submit([inode = _inode, flags = _flags, path = std::move(path)](InMemoryState& state) {
                return checkRights(flags, Operation::RemoveDirectoryAt)
                    .and_then([&] {
                        return state.walk(inode, path, SymlinkPolicy::NoFollow, TrailingSlashOnLink::Refuse);
                    })
                    .and_then([&](Location const& location) -> FsResult<void> {
                        if (!location.node)
                            return refuse(FsErrorCode::NoEntry);
                        if (location.name.empty())
                            return refuse(FsErrorCode::Invalid);
                        auto const& node = state.nodes.at(*location.node);
                        if (node.type != DescriptorType::Directory)
                            return refuse(FsErrorCode::NotDirectory);
                        if (!node.entries.empty())
                            return refuse(FsErrorCode::NotEmpty);
                        state.erase(location.parent(), location.name);
                        state.dropLink(*location.node);
                        return {};
                    });
            });
        }

        async::Task<FsResult<void>> renameAt(std::string oldPath,
                                             IDescriptor* newDirectory,
                                             std::string newPath) override
        {
            auto const target = sameSystemTarget(newDirectory);
            return submit([inode = _inode,
                           flags = _flags,
                           oldPath = std::move(oldPath),
                           target,
                           newPath = std::move(newPath)](InMemoryState& state) -> FsResult<void> {
                if (auto checked = checkRights(flags, Operation::RenameAt); !checked)
                    return checked;
                if (!target)
                    return std::unexpected { target.error() };
                if (auto checked = checkRights(target->flags, Operation::RenameAt); !checked)
                    return checked;
                auto from = state.walk(inode, oldPath, SymlinkPolicy::NoFollow, TrailingSlashOnLink::Refuse);
                if (!from)
                    return std::unexpected { std::move(from.error()) };
                auto to =
                    state.walk(target->inode, newPath, SymlinkPolicy::NoFollow, TrailingSlashOnLink::Refuse);
                if (!to)
                    return std::unexpected { std::move(to.error()) };
                return renameLocation(state, *from, *to);
            });
        }

        async::Task<FsResult<void>> symlinkAt(std::string target, std::string newPath) override
        {
            return submit(
                [inode = _inode, flags = _flags, target = std::move(target), newPath = std::move(newPath)](
                    InMemoryState& state) -> FsResult<void> {
                    if (auto checked = checkRights(flags, Operation::SymlinkAt); !checked)
                        return checked;
                    if (auto checked = fs::detail::checkSymlinkTarget(target, *state.policy); !checked)
                        return checked;
                    return state.vacant(inode, newPath, DescriptorType::SymbolicLink)
                        .transform([&](Location const& location) {
                            auto const link = state.createNode(DescriptorType::SymbolicLink);
                            state.nodes.at(link).target = target;
                            state.insert(location.parent(), location.name, link);
                        });
                });
        }

        async::Task<FsResult<void>> unlinkFileAt(std::string path) override
        {
            return submit([inode = _inode, flags = _flags, path = std::move(path)](InMemoryState& state) {
                return checkRights(flags, Operation::UnlinkFileAt)
                    .and_then([&] {
                        return state.walk(inode, path, SymlinkPolicy::NoFollow, TrailingSlashOnLink::Refuse);
                    })
                    .and_then([&](Location const& location) -> FsResult<void> {
                        if (!location.node)
                            return refuse(FsErrorCode::NoEntry);
                        if (location.name.empty()
                            || state.nodes.at(*location.node).type == DescriptorType::Directory)
                            return refuse(FsErrorCode::IsDirectory);
                        state.erase(location.parent(), location.name);
                        state.dropLink(*location.node);
                        return {};
                    });
            });
        }

        async::Task<FsResult<MetadataHash>> metadataHashAt(SymlinkPolicy policy, std::string path) override
        {
            return submit(
                [inode = _inode, flags = _flags, policy, path = std::move(path)](InMemoryState& state) {
                    return checkRights(flags, Operation::MetadataHashAt)
                        .and_then([&] { return state.existing(inode, path, policy); })
                        .transform([&](std::uint64_t found) {
                            return MetadataHash { .lower = found, .upper = state.nodes.at(found).changes };
                        });
                });
        }

        async::Task<FsResult<bool>> isSameObject(IDescriptor* other) override
        {
            auto target = sameSystemTarget(other);
            return submit([inode = _inode, target = std::move(target)](InMemoryState&) -> FsResult<bool> {
                if (target)
                    return target->inode == inode;
                if (target.error().code == FsErrorCode::CrossDevice)
                    return false;
                return std::unexpected { target.error() };
            });
        }

      private:
        /// A directory passed as `newDirectory`: the node and the rights, read on the caller's thread.
        struct Target
        {
            std::uint64_t inode = 0;
            DescriptorFlags flags = DescriptorFlags::None;
        };

        /// @return @p other's node and rights; `CrossDevice` unless it is a descriptor of this very
        ///         model, `BadDescriptor` for none at all.
        [[nodiscard]] FsResult<Target> sameSystemTarget(IDescriptor* other) const
        {
            if (other == nullptr)
                return refuse(FsErrorCode::BadDescriptor);
            auto const* mine = dynamic_cast<InMemoryDescriptor const*>(other);
            if (mine == nullptr || mine->_state != _state)
                return refuse(FsErrorCode::CrossDevice);
            return Target { .inode = mine->_inode, .flags = mine->_flags };
        }

        /// @return How an `openAt` resolves its final component: `O_CREAT | O_EXCL` never follows a
        ///         final symbolic link, so a dangling one is `Exist` rather than created through.
        static SymlinkPolicy finalPolicy(SymlinkPolicy policy, OpenFlags open) noexcept
        {
            if (hasFlag(open, OpenFlags::Create) && hasFlag(open, OpenFlags::Exclusive))
                return SymlinkPolicy::NoFollow;
            return policy;
        }

        /// The node an `openAt` opens, creating or truncating it as @p open says.
        static FsResult<std::uint64_t> openLocation(InMemoryState& state,
                                                    Location const& location,
                                                    OpenFlags open,
                                                    DescriptorFlags childFlags)
        {
            if (!location.node)
            {
                if (!hasFlag(open, OpenFlags::Create))
                    return refuse(FsErrorCode::NoEntry);
                if (location.finalComponent == fs::detail::FinalComponent::Directory)
                    return refuse(FsErrorCode::NotDirectory);
                auto const file =
                    state.createNode(DescriptorType::RegularFile,
                                     hasFlag(open, OpenFlags::OwnerOnly) ? CreationAccess::OwnerOnly
                                                                         : CreationAccess::Default);
                state.insert(location.parent(), location.name, file);
                return file;
            }
            if (hasFlag(open, OpenFlags::Create) && hasFlag(open, OpenFlags::Exclusive))
                return refuse(FsErrorCode::Exist);
            auto& node = state.nodes.at(*location.node);
            if (node.type == DescriptorType::SymbolicLink)
                return refuse(FsErrorCode::Loop);
            if (node.type == DescriptorType::Directory)
            {
                if (hasFlag(childFlags, DescriptorFlags::Write) || hasFlag(open, OpenFlags::Truncate)
                    || hasFlag(open, OpenFlags::Create))
                    return refuse(FsErrorCode::IsDirectory);
            }
            else if (hasFlag(open, OpenFlags::Directory))
                return refuse(FsErrorCode::NotDirectory);
            if (hasFlag(open, OpenFlags::Truncate) && !node.data.empty())
            {
                node.data.clear();
                state.touch(node, state.now());
            }
            return *location.node;
        }

        /// Moves @p from to @p to, replacing what @p to names where POSIX `rename` would.
        static FsResult<void> renameLocation(InMemoryState& state, Location const& from, Location const& to)
        {
            if (!from.node)
                return refuse(FsErrorCode::NoEntry);
            if (from.name.empty() || to.name.empty())
                return refuse(FsErrorCode::Invalid);
            auto const moving = *from.node;
            auto const isDirectory = state.nodes.at(moving).type == DescriptorType::Directory;
            if (isDirectory && std::ranges::find(to.directories, moving) != to.directories.end())
                return refuse(FsErrorCode::Invalid);
            if (!isDirectory && to.finalComponent == fs::detail::FinalComponent::Directory)
                return refuse(FsErrorCode::NotDirectory);
            if (to.node == moving)
                return {};
            if (to.node)
            {
                auto const& replaced = state.nodes.at(*to.node);
                auto const replacesDirectory = replaced.type == DescriptorType::Directory;
                if (isDirectory && !replacesDirectory)
                    return refuse(FsErrorCode::NotDirectory);
                if (!isDirectory && replacesDirectory)
                    return refuse(FsErrorCode::IsDirectory);
                if (replacesDirectory && !replaced.entries.empty())
                    return refuse(FsErrorCode::NotEmpty);
                state.dropLink(*to.node);
            }
            state.erase(from.parent(), from.name);
            state.insert(to.parent(), to.name, moving);
            return {};
        }

        std::shared_ptr<InMemoryState> _state;
        std::uint64_t _inode;
        DescriptorFlags _flags;
    };

} // namespace

InMemoryDescriptorFileSystem::InMemoryDescriptorFileSystem(async::IExecutor& home,
                                                           async::IExecutor& blocking,
                                                           platform::IWallClock const& clock,
                                                           PathPolicy const& policy,
                                                           std::size_t maxReadChunk,
                                                           IdentityModel identities,
                                                           std::size_t maxAncestorDepth):
    _state { std::make_shared<InMemoryState>(
        home, blocking, clock, policy, maxReadChunk, identities, maxAncestorDepth) }
{
}

InMemoryDescriptorFileSystem::~InMemoryDescriptorFileSystem() = default;

void InMemoryDescriptorFileSystem::addDirectory(std::string_view path)
{
    auto const lock = std::scoped_lock { _state->mutex };
    std::ignore = _state->add(path, DescriptorType::Directory);
}

void InMemoryDescriptorFileSystem::addFile(std::string_view path, std::string_view content)
{
    auto const lock = std::scoped_lock { _state->mutex };
    auto& node = _state->nodes.at(_state->add(path, DescriptorType::RegularFile));
    node.data.resize(content.size());
    std::ranges::transform(content, node.data.begin(), [](char c) { return static_cast<std::byte>(c); });
}

void InMemoryDescriptorFileSystem::setAccessFacts(std::string_view path, AccessFacts facts)
{
    auto const lock = std::scoped_lock { _state->mutex };
    auto node = RootInode;
    for (auto const piece: path | std::views::split('/'))
    {
        auto const name = std::string_view { piece };
        if (name.empty() || name == ".")
            continue;
        auto const found = _state->lookup(node, name);
        if (!found)
            throw std::invalid_argument { "InMemoryDescriptorFileSystem::setAccessFacts: no '"
                                          + std::string { path } + "'" };
        node = *found;
    }
    _state->nodes.at(node).access = std::move(facts);
}

void InMemoryDescriptorFileSystem::addSymlink(std::string_view path, std::string target)
{
    auto const lock = std::scoped_lock { _state->mutex };
    _state->nodes.at(_state->add(path, DescriptorType::SymbolicLink)).target = std::move(target);
}

async::Task<FsResult<std::unique_ptr<IDescriptor>>> InMemoryDescriptorFileSystem::openPreopen(
    std::filesystem::path hostDirectory, DescriptorFlags flags)
{
    return async::offload(_state->home,
                          _state->blocking,
                          [state = _state,
                           path = hostDirectory.generic_string(),
                           flags]() -> FsResult<std::unique_ptr<IDescriptor>> {
                              if (hasFlag(flags, DescriptorFlags::Write))
                                  return refuse(FsErrorCode::IsDirectory);
                              auto const lock = std::scoped_lock { state->mutex };
                              auto directory = RootInode;
                              for (auto const piece: path | std::views::split('/'))
                              {
                                  auto const name = std::string_view { piece };
                                  if (name.empty() || name == ".")
                                      continue;
                                  auto const found = state->lookup(directory, name);
                                  if (!found)
                                      return refuse(FsErrorCode::NoEntry);
                                  if (state->nodes.at(*found).type != DescriptorType::Directory)
                                      return refuse(FsErrorCode::NotDirectory);
                                  directory = *found;
                              }
                              return std::make_unique<InMemoryDescriptor>(state, directory, flags);
                          });
}

async::Task<FsResult<Relation>> InMemoryDescriptorFileSystem::relation(IDescriptor const& first,
                                                                       IDescriptor const& second)
{
    return async::offload(_state->home,
                          _state->blocking,
                          [state = _state,
                           firstNode = InMemoryDescriptor::nodeOf(first, _state),
                           secondNode = InMemoryDescriptor::nodeOf(second, _state)]() -> FsResult<Relation> {
                              if (!firstNode || !secondNode)
                                  return refuse(FsErrorCode::CrossDevice);
                              auto const lock = std::scoped_lock { state->mutex };
                              if (state->nodes.at(*firstNode).type != DescriptorType::Directory
                                  || state->nodes.at(*secondNode).type != DescriptorType::Directory)
                                  return refuse(FsErrorCode::NotDirectory);
                              if (state->identities == IdentityModel::Unavailable)
                                  return refuse(FsErrorCode::Unsupported);
                              if (*firstNode == *secondNode)
                                  return Relation::Same;
                              return state->ancestryOf(*firstNode).and_then([&](auto const& up) {
                                  return state->ancestryOf(*secondNode).transform([&](auto const& down) {
                                      return fs::detail::decideRelation(up, down);
                                  });
                              });
                          });
}

} // namespace core::fs::testing
