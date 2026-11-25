// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Handing a handle Contour owns to an extension host process that runs already, with the request that
/// needs it: the shared memory object an Instantiate names.
///
/// On POSIX the descriptor travels with the request's frame (SCM_RIGHTS), and the PDU names it by 0. On
/// Windows Contour duplicates the handle into the helper (DuplicateHandle), through a handle on the helper
/// with PROCESS_DUP_HANDLE access that the launcher hands out, and the PDU carries the duplicate's value.
///
/// A grant whose request is never sent is lost with the helper: a failed send ends the incarnation. A forged
/// one is the helper's to refuse: what it cannot map as the object it was promised is a protocol violation,
/// and on Windows a value it cannot map is released rather than closed, as it may name a handle of its own.

#include <cstdint>
#include <expected>
#include <optional>

#include <vtextension/wasm/remote/transport/Channel.hpp>
#include <vtextension/wasm/remote/transport/OwnedHandle.hpp>
#include <vtextension/wasm/remote/transport/PduStream.hpp>

namespace vtextension::wasm::remote::transport
{

/// The process a handle is granted to. On POSIX it names nothing: a descriptor travels with its frame and is
/// installed by whichever process reads it, so that every target is currentProcess(). On Windows it is the
/// process the handle is duplicated into. Move-only.
class HandleTarget
{
  public:
    HandleTarget(HandleTarget&&) noexcept = default;
    HandleTarget& operator=(HandleTarget&&) noexcept = default;
    HandleTarget(HandleTarget const&) = delete;
    HandleTarget& operator=(HandleTarget const&) = delete;
    ~HandleTarget() = default;

    /// @return This process: what a host serving in this process is granted handles as.
    [[nodiscard]] static HandleTarget currentProcess() noexcept;

#ifdef _WIN32
    /// @param process A handle on the process, with PROCESS_DUP_HANDLE access; the target's own.
    /// @return That process.
    [[nodiscard]] static HandleTarget ofProcess(OwnedHandle process) noexcept;

    /// @return The handle a duplicate is made into the target through: this process's pseudo-handle for
    ///         currentProcess().
    [[nodiscard]] NativeHandle process() const noexcept;
#endif

  private:
#ifdef _WIN32
    /// @param process The process's handle; none for this process.
    explicit HandleTarget(OwnedHandle process) noexcept;

    OwnedHandle _process; ///< A handle on the target; none for this process.
#else
    HandleTarget() noexcept = default;
#endif
};

/// A handle granted to a target, ready to travel with the PDU that names it.
struct HandleGrant
{
    /// What the PDU names the handle by: on Windows the duplicate's value in the target; on POSIX 0.
    uint64_t wireValue = 0;
    /// What travels with the PDU's frame: on POSIX the descriptor, still the granter's; nothing on Windows.
    std::optional<NativeHandle> attached;
};

/// Why a handle could not be granted.
enum class GrantError : uint8_t
{
    Io, ///< The system refused to duplicate it into the target (Windows).
};

/// Grants @p object to @p target. On Windows the duplicate has FILE_MAP_READ | FILE_MAP_WRITE access and is
/// the target's from now on, whether or not the PDU naming it is sent; on POSIX nothing happens before the
/// PDU is sent (PduStream::send(pdu, grant)).
/// @param target Who it is granted to.
/// @param object The handle; still the caller's.
/// @return The grant; `Io` if the system refused.
[[nodiscard]] std::expected<HandleGrant, GrantError> grantHandle(HandleTarget const& target,
                                                                 NativeHandle object);

/// Claims the handle a PDU just received from @p stream names by @p wireValue: on POSIX the descriptor that
/// arrived with it, which @p stream held; on Windows the value itself, which the caller must check by using
/// it, and release rather than close if it cannot.
/// @param stream The helper's control stream, which received the PDU.
/// @param wireValue What the PDU names the handle by.
/// @return The handle, now the caller's; `Malformed` if no handle came with the PDU, or (POSIX) the value is
///         not 0, or (Windows) it is 0 or names no handle this process could have.
[[nodiscard]] std::expected<OwnedHandle, StreamError> claimGrantedHandle(PduStream& stream,
                                                                         uint64_t wireValue);

} // namespace vtextension::wasm::remote::transport
