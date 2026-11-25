// SPDX-License-Identifier: Apache-2.0
//
// Grants on POSIX: the descriptor itself travels with the frame of the PDU that names it, and the reader's
// stream holds it until it is claimed.
#include <cstdint>
#include <expected>

#include <vtextension/wasm/remote/transport/HandleGrant.hpp>

namespace vtextension::wasm::remote::transport
{

std::expected<HandleGrant, GrantError> grantHandle(HandleTarget const& target, NativeHandle object)
{
    if (target.isNone())
        return std::unexpected(GrantError::Io);
    return HandleGrant { .wireValue = 0, .attached = object };
}

std::expected<GrantedHandle, StreamError> claimGrantedHandle(PduStream& stream, uint64_t wireValue)
{
    auto handle = stream.takeAttachedHandle();
    // Taken before the value is looked at, so that a refused claim leaves nothing behind for the next one.
    if (wireValue != 0 || !handle)
        return std::unexpected(StreamError::Malformed);
    return GrantedHandle { handle.release() };
}

void GrantedHandle::letGo() noexcept
{
    // A descriptor this process was given: its own to close, whatever it names.
    if (_handle != NoNativeHandle)
        closeNativeHandle(std::exchange(_handle, NoNativeHandle));
}

} // namespace vtextension::wasm::remote::transport
