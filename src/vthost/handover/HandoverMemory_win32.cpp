// SPDX-License-Identifier: Apache-2.0
#include <vthost/handover/HandoverMemory.hpp>

#ifdef _WIN32

namespace vthost::handover
{

namespace
{
    /// Windows has no in-place exec to hand anything across, so there is no memory to hand it in.
    class UnsupportedHandoverMemory final: public HandoverMemory
    {
      public:
        [[nodiscard]] std::expected<int, HandoverError> write(std::span<uint8_t const> /*image*/) override
        {
            return std::unexpected { HandoverError::NotSupported };
        }

        [[nodiscard]] std::expected<std::vector<uint8_t>, HandoverError> read(int /*fd*/) override
        {
            return std::unexpected { HandoverError::NotSupported };
        }

        [[nodiscard]] std::expected<void, HandoverError> keepAcrossExec(int /*fd*/) override
        {
            return std::unexpected { HandoverError::NotSupported };
        }
    };
} // namespace

std::unique_ptr<HandoverMemory> makeNativeHandoverMemory()
{
    return std::make_unique<UnsupportedHandoverMemory>();
}

} // namespace vthost::handover

#endif
