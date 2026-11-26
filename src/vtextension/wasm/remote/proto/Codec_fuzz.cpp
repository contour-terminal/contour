// SPDX-License-Identifier: Apache-2.0
//
// libFuzzer target for the extension host protocol's codec (VTEXTENSION_FUZZING). Decodes its input as a
// frame arriving each way on each channel; whatever decodes must encode again to a frame that decodes to the
// same PDU.
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include <vtextension/wasm/remote/proto/Codec.hpp>

using namespace vtextension::wasm::remote::proto;

extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, std::size_t size)
{
    auto const input = std::span { data, size };
    // Small limits, so that the fuzzer reaches the caps that follow them.
    auto const limits = PduLimits { .maxModuleBytes = 4096, .maxMemoryBytes = 4096 };
    for (auto const direction: std::array { Direction::ToHelper, Direction::ToParent })
    {
        for (auto const channel: std::array { ChannelKind::Control, ChannelKind::Lane })
        {
            auto const decoded = decodeFrame(input, direction, channel, limits);
            if (!decoded)
                continue;
            if (decoded->consumed > input.size())
                __builtin_trap();
            // What decoded is within every cap, so it encodes.
            auto const frame = encodeFrame(decoded->pdu);
            if (!frame)
                __builtin_trap();
            auto const again = decodeFrame(*frame, direction, channel, limits);
            if (!again || again->pdu != decoded->pdu)
                __builtin_trap();
        }
    }
    return 0;
}
