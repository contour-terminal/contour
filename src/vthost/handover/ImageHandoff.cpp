// SPDX-License-Identifier: Apache-2.0
#include <vthost/handover/ImageHandoff.hpp>

#include <cstddef>
#include <span>
#include <system_error>
#include <utility>

#include <vthost/DaemonBinary.hpp>
#include <vthost/Logging.hpp>
#include <vthost/handover/HandoverMemory.hpp>

namespace vthost::handover
{

namespace
{
    /// The descriptors kept across an exec, and the memory object the image is in: made close-on-exec again,
    /// and the memory object discarded, as it goes. Only an exec that succeeded never lets it go.
    class KeptAcrossExec
    {
      public:
        /// @param memory What keeps the descriptors, and holds the image.
        /// @param image The memory object the image was written into, which this discards.
        /// @param descriptors The descriptors to keep (keepAll()). Not owned; they outlive this.
        KeptAcrossExec(HandoverMemory& memory,
                       vtpty::CrossingHandle image,
                       std::span<vtpty::CrossingHandle const> descriptors) noexcept:
            _memory { memory }, _image { image }, _descriptors { descriptors }
        {
        }
        KeptAcrossExec(KeptAcrossExec const&) = delete;
        KeptAcrossExec(KeptAcrossExec&&) = delete;
        KeptAcrossExec& operator=(KeptAcrossExec const&) = delete;
        KeptAcrossExec& operator=(KeptAcrossExec&&) = delete;
        ~KeptAcrossExec()
        {
            for (auto const descriptor: _descriptors.first(_keptCount))
                if (!_memory.closeOnExecAgain(descriptor))
                    errorLog()("live upgrade: descriptor {} stays inheritable after the failed exec",
                               descriptor.value);
            _memory.discard(_image);
        }

        /// Keeps every descriptor across the exec, in order, up to the first that cannot be kept.
        /// @return Nothing, or why one could not be kept.
        [[nodiscard]] std::expected<void, HandoverError> keepAll()
        {
            for (auto const descriptor: _descriptors)
            {
                if (auto kept = _memory.keepAcrossExec(descriptor); !kept)
                    return kept;
                ++_keptCount;
            }
            return {};
        }

      private:
        HandoverMemory& _memory;
        vtpty::CrossingHandle _image;
        std::span<vtpty::CrossingHandle const> _descriptors;
        std::size_t _keptCount = 0; ///< How many of _descriptors, from the first, are kept.
    };
} // namespace

ExecFailure ExecHandoff::handOver(std::filesystem::path const& executable,
                                  std::vector<std::string> argv,
                                  std::vector<vtpty::CrossingHandle> crossing,
                                  std::vector<uint8_t> bytes)
{
    // Room for the image's own descriptor made before it exists: nothing between writing the memory object
    // and handing it to the guard that discards it may throw.
    crossing.reserve(crossing.size() + 1);
    // The encoded image goes as soon as the memory object holds it, as it would otherwise stay on beside it
    // after an exec that failed.
    auto const memoryHandle = _memory.write(std::exchange(bytes, {}));
    if (!memoryHandle)
        return ExecFailure { .cause = std::make_error_code(std::errc::not_enough_memory) };
    crossing.push_back(*memoryHandle);
    auto kept = KeptAcrossExec { _memory, *memoryHandle, crossing };
    if (!kept.keepAll())
        return ExecFailure { .cause = std::make_error_code(std::errc::bad_file_descriptor) };

    // Returns only on failure.
    argv.push_back(handoverArgument(handoverToken(*memoryHandle)));
    auto const failure = _replacer.replaceWith(executable, argv);
    return ExecFailure { .cause = failure ? failure : std::make_error_code(std::errc::io_error) };
}

} // namespace vthost::handover
