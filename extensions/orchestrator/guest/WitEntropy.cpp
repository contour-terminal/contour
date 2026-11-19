// SPDX-License-Identifier: Apache-2.0
//
// The guest's one source of randomness, read through WASI. WebAssembly only.
#include <orchestrator/guest/WitEntropy.hpp>

#include <array>
#include <cstdint>
#include <cstdlib>

#include <wasi/api.h>

namespace orchestrator::guest
{

namespace
{
    /// Session UUIDs from the host's entropy (WASI `random_get`, on the host's allow list).
    class WitEntropy final: public IUuidSource
    {
      public:
        [[nodiscard]] std::array<uint8_t, 16> next() override
        {
            auto bytes = std::array<uint8_t, 16> {};
            // A session UUID that is not random could name another agent's session: a guest that cannot
            // read entropy faults rather than dispatch with one.
            if (__wasi_random_get(bytes.data(), bytes.size()) != __WASI_ERRNO_SUCCESS)
                std::abort();
            return bytes;
        }
    };
} // namespace

IUuidSource& witEntropy()
{
    static auto instance = WitEntropy {};
    return instance;
}

} // namespace orchestrator::guest
