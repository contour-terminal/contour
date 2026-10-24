// SPDX-License-Identifier: Apache-2.0
#include <vthost/proto/Unsupported.hpp>

#include <utility>
#include <variant>

namespace vthost::proto
{

UnsupportedHandling UnsupportedTracker::handle(DecodedFrame const& frame)
{
    if (auto const* unknown = std::get_if<Invalid>(&frame.pdu))
        return { .verdict = UnsupportedVerdict::Answer,
                 .pdu = Unsupported { .refusedSerial = frame.serial, .ident = unknown->ident } };
    if (auto const* refusal = std::get_if<Unsupported>(&frame.pdu))
    {
        auto const inserted = _unsupportedByPeer.insert(refusal->ident).second;
        return { .verdict = inserted ? UnsupportedVerdict::Refused : UnsupportedVerdict::AlreadyRefused,
                 .pdu = *refusal };
    }
    return {};
}

bool UnsupportedTracker::peerRefused(PduType type) const noexcept
{
    return _unsupportedByPeer.contains(std::to_underlying(type));
}

} // namespace vthost::proto
