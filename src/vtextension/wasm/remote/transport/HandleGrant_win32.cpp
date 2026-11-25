// SPDX-License-Identifier: Apache-2.0
//
// Grants on Windows: the handle is duplicated into the target, and the PDU carries the duplicate's value.
#include <bit>
#include <cstdint>
#include <expected>
#include <limits>
#include <utility>

#include <vtextension/wasm/remote/transport/HandleGrant.hpp>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace vtextension::wasm::remote::transport
{

HandleTarget::HandleTarget(OwnedHandle process) noexcept: _process { std::move(process) }
{
}

HandleTarget HandleTarget::currentProcess() noexcept
{
    return HandleTarget { OwnedHandle {} };
}

HandleTarget HandleTarget::ofProcess(OwnedHandle process) noexcept
{
    return HandleTarget { std::move(process) };
}

NativeHandle HandleTarget::process() const noexcept
{
    return _process ? _process.get() : ::GetCurrentProcess();
}

std::expected<HandleGrant, GrantError> grantHandle(HandleTarget const& target, NativeHandle object)
{
    auto duplicate = HANDLE { nullptr };
    if (!::DuplicateHandle(::GetCurrentProcess(),
                           object,
                           target.process(),
                           &duplicate,
                           FILE_MAP_READ | FILE_MAP_WRITE,
                           FALSE,
                           0))
        return std::unexpected(GrantError::Io);
    return HandleGrant { .wireValue = static_cast<uint64_t>(std::bit_cast<uintptr_t>(duplicate)),
                         .attached = std::nullopt };
}

std::expected<OwnedHandle, StreamError> claimGrantedHandle(PduStream& stream, uint64_t wireValue)
{
    // Nothing travels beside a frame on Windows; a stream that holds something was not given it by a peer.
    if (stream.takeAttachedHandle())
        return std::unexpected(StreamError::Malformed);
    if (wireValue == 0 || wireValue > std::numeric_limits<uintptr_t>::max())
        return std::unexpected(StreamError::Malformed);
    return OwnedHandle { std::bit_cast<HANDLE>(static_cast<uintptr_t>(wireValue)) };
}

} // namespace vtextension::wasm::remote::transport
