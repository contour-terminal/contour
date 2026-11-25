// SPDX-License-Identifier: Apache-2.0
//
// Grants on Windows: the handle is duplicated into the target, and the PDU carries the duplicate's value.
#include <bit>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <utility>

#include <vtextension/wasm/remote/transport/HandleGrant.hpp>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace vtextension::wasm::remote::transport
{

HandleTarget::HandleTarget(OwnedHandle process) noexcept:
    _kind { Kind::OtherProcess }, _process { std::move(process) }
{
}

HandleTarget HandleTarget::ofProcess(OwnedHandle process) noexcept
{
    return HandleTarget { std::move(process) };
}

std::optional<NativeHandle> HandleTarget::process() const noexcept
{
    switch (_kind)
    {
        case Kind::None: return std::nullopt;
        case Kind::CurrentProcess: return ::GetCurrentProcess();
        case Kind::OtherProcess: return _process.get();
    }
    return std::nullopt;
}

std::expected<HandleGrant, GrantError> grantHandle(HandleTarget const& target, NativeHandle object)
{
    auto const process = target.process();
    if (!process)
        return std::unexpected(GrantError::Io);
    auto duplicate = HANDLE { nullptr };
    if (!::DuplicateHandle(
            ::GetCurrentProcess(), object, *process, &duplicate, FILE_MAP_READ | FILE_MAP_WRITE, FALSE, 0))
        return std::unexpected(GrantError::Io);
    return HandleGrant { .wireValue = static_cast<uint64_t>(std::bit_cast<uintptr_t>(duplicate)),
                         .attached = std::nullopt };
}

std::expected<GrantedHandle, StreamError> claimGrantedHandle(PduStream& stream, uint64_t wireValue)
{
    // Nothing travels beside a frame on Windows; a stream that holds something was not given it by a peer.
    if (stream.takeAttachedHandle())
        return std::unexpected(StreamError::Malformed);
    // A handle's value is a multiple of 4, and positive: the negative ones are pseudo-handles (this process,
    // this thread, their tokens), which name no object granted.
    if (wireValue == 0 || wireValue % 4 != 0 || wireValue > std::numeric_limits<uintptr_t>::max()
        || static_cast<intptr_t>(static_cast<uintptr_t>(wireValue)) < 0)
        return std::unexpected(StreamError::Malformed);
    return GrantedHandle { std::bit_cast<HANDLE>(static_cast<uintptr_t>(wireValue)) };
}

void GrantedHandle::letGo() noexcept
{
    // A value a peer chose may name a handle of this process's own: forgotten, never closed.
    _handle = NoNativeHandle;
}

} // namespace vtextension::wasm::remote::transport
