// SPDX-License-Identifier: Apache-2.0
#include <core/fs/detail/NativeSupport.hpp>

#include <core/Sha256.hpp>
#include <core/fs/FsError.hpp>
#include <core/fs/PathPolicy.hpp>
#include <core/fs/detail/Ancestry.hpp>
#include <core/fs/detail/PathWalk.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <ranges>
#include <span>
#include <utility>

namespace core::fs::detail
{

namespace
{

    /// Eight 64-bit words: device, the two halves of the file id, size, and two times of two words.
    constexpr std::size_t MessageSize = 8 * sizeof(std::uint64_t);

    /// The bytes @c hashMetadata hashes: every field, little-endian, in declaration order.
    class MessageWriter
    {
      public:
        void put(std::uint64_t value) noexcept
        {
            for (auto const shift: std::views::iota(0U, 8U))
                _bytes.at(_size++) = static_cast<std::byte>((value >> (shift * 8U)) & 0xFFU);
        }

        void put(FileTime time) noexcept
        {
            put(static_cast<std::uint64_t>(time.seconds));
            put(std::uint64_t { time.nanoseconds });
        }

        [[nodiscard]] std::span<std::byte const> bytes() const noexcept { return { _bytes.data(), _size }; }

      private:
        std::array<std::byte, MessageSize> _bytes {};
        std::size_t _size = 0;
    };

    std::uint64_t littleEndianAt(Sha256::Digest const& digest, std::size_t offset) noexcept
    {
        auto value = std::uint64_t { 0 };
        for (auto const index: std::views::iota(std::size_t { 0 }, std::size_t { 8 }))
            value |= std::to_integer<std::uint64_t>(digest.at(offset + index)) << (index * 8U);
        return value;
    }

} // namespace

std::unexpected<FsError> refuse(FsErrorCode code)
{
    return std::unexpected { FsError { .code = code, .systemCode = 0, .context = {} } };
}

FsResult<void> checkOptions(NativeFileSystemOptions const& options)
{
    if (options.maxReadChunk == 0)
        return std::unexpected { FsError {
            .code = FsErrorCode::Invalid, .systemCode = 0, .context = "maxReadChunk must be above 0" } };
    return {};
}

NativeContext::NativeContext(async::IExecutor& homeExecutor,
                             async::IExecutor& blockingExecutor,
                             platform::IWallClock const& clock,
                             IRandom& random,
                             NativeFileSystemOptions fileSystemOptions,
                             std::string_view separators,
                             INtFiles const* files,
                             std::shared_ptr<OwnerSecurity const> security,
                             std::shared_ptr<OwnerOnlyCreator const> creator,
                             std::shared_ptr<IMountBoundaries const> boundaries):
    home { &homeExecutor },
    blocking { &blockingExecutor },
    wallClock { &clock },
    policy { fileSystemOptions.policy != nullptr ? fileSystemOptions.policy : &nativePathPolicy() },
    kernelSeparators { separators },
    options { fileSystemOptions },
    // The kernel counts the links before the final name against its own 40: a lower limit is kept
    // only by the walk.
    beneathOpener { fileSystemOptions.strategy == BeneathStrategy::Best
                            && fileSystemOptions.maxSymlinkHops >= DefaultMaxSymlinkHops
                        ? makeBestBeneathOpener()
                        : nullptr },
    ntFiles { files },
    ownerSecurity { std::move(security) },
    ownerOnlyCreator { std::move(creator) },
    mountBoundaries { std::move(boundaries) }
{
    random.fill(secret);
}

MetadataHash hashMetadata(MetadataSecret const& secret, MetadataFields const& fields) noexcept
{
    auto message = MessageWriter {};
    message.put(fields.device);
    message.put(fields.fileIdLow);
    message.put(fields.fileIdHigh);
    message.put(fields.size);
    message.put(fields.modified);
    message.put(fields.statusChanged);
    auto const digest = hmacSha256(secret, message.bytes());
    return MetadataHash { .lower = littleEndianAt(digest, 0), .upper = littleEndianAt(digest, 8) };
}

FileTime fileTimeOf(std::chrono::system_clock::time_point when) noexcept
{
    auto const sinceEpoch = when.time_since_epoch();
    auto const seconds = std::chrono::floor<std::chrono::seconds>(sinceEpoch);
    return FileTime {
        .seconds = seconds.count(),
        .nanoseconds = static_cast<std::uint32_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(sinceEpoch - seconds).count())
    };
}

FsResult<void> requireDirectory(DescriptorType type)
{
    if (type != DescriptorType::Directory)
        return refuse(FsErrorCode::NotDirectory);
    return {};
}

FsResult<NativeDescriptorBase::Target> NativeDescriptorBase::targetIn(SharedContext const& context,
                                                                      IDescriptor const& descriptor)
{
    auto const* mine = dynamic_cast<NativeDescriptorBase const*>(&descriptor);
    if (mine == nullptr || mine->_context != context)
        return refuse(FsErrorCode::CrossDevice);
    return Target { .handle = mine->_handle, .flags = mine->_flags, .type = mine->_type };
}

FsResult<NativeDescriptorBase::Target> NativeDescriptorBase::sameSystemTarget(IDescriptor* other) const
{
    if (other == nullptr)
        return refuse(FsErrorCode::BadDescriptor);
    return targetIn(_context, *other);
}

FsResult<std::optional<NativeDescriptorBase::Target>> NativeDescriptorBase::sameObjectCandidate(
    IDescriptor* other) const
{
    auto target = sameSystemTarget(other);
    if (target)
        return std::optional { std::move(*target) };
    if (target.error().code == FsErrorCode::CrossDevice)
        return std::optional<Target> {};
    return std::unexpected { std::move(target.error()) };
}

async::Task<FsResult<Relation>> relationOf(SharedContext const& context,
                                           IDescriptor const& first,
                                           IDescriptor const& second)
{
    using Target = NativeDescriptorBase::Target;
    auto targets = NativeDescriptorBase::targetIn(context, first).and_then([&](Target firstTarget) {
        return NativeDescriptorBase::targetIn(context, second).transform([&](Target secondTarget) {
            return std::pair { std::move(firstTarget), std::move(secondTarget) };
        });
    });
    return async::offload(
        context->home, context->blocking, [context, targets = std::move(targets)]() -> FsResult<Relation> {
            if (!targets)
                return std::unexpected { targets.error() };
            auto const& [firstTarget, secondTarget] = *targets;
            if (firstTarget.type != DescriptorType::Directory
                || secondTarget.type != DescriptorType::Directory)
                return refuse(FsErrorCode::NotDirectory);
            return nativeRelation(*context, firstTarget.handle->get(), secondTarget.handle->get());
        });
}

WalkRules walkRulesOf(NativeContext const& context,
                      SymlinkPolicy finalPolicy,
                      TrailingSlashOnLink onLink) noexcept
{
    return WalkRules { .policy = context.policy,
                       .finalPolicy = finalPolicy,
                       .onLink = onLink,
                       .maxSymlinkHops = context.options.maxSymlinkHops,
                       .kernelSeparators = context.kernelSeparators };
}

} // namespace core::fs::detail
