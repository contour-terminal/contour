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
#include <utility>

#include <vtextension/wasm/remote/transport/Channel.hpp>
#include <vtextension/wasm/remote/transport/OwnedHandle.hpp>
#include <vtextension/wasm/remote/transport/PduStream.hpp>

namespace vtextension::wasm::remote::transport
{

/// The process a handle is granted to. On POSIX it names nothing but whether a handle may be granted: a
/// descriptor travels with its frame and is installed by whichever process reads it. On Windows it is the
/// process the handle is duplicated into. Move-only: a moved-from target is none(), which grants nothing.
class HandleTarget
{
  public:
    HandleTarget(HandleTarget&& other) noexcept:
        _kind { std::exchange(other._kind, Kind::None) }
#ifdef _WIN32
        ,
        _process { std::move(other._process) }
#endif
    {
    }

    HandleTarget& operator=(HandleTarget&& other) noexcept
    {
        if (this != &other)
        {
            _kind = std::exchange(other._kind, Kind::None);
#ifdef _WIN32
            _process = std::move(other._process);
#endif
        }
        return *this;
    }

    HandleTarget(HandleTarget const&) = delete;
    HandleTarget& operator=(HandleTarget const&) = delete;
    ~HandleTarget() = default;

    /// This process: what a host serving in this process is granted handles as, and on POSIX every helper.
    /// On Windows a handle granted to it is a duplicate in this process, which nothing but a claim
    /// (claimGrantedHandle()) closes: whoever grants to it must see the grant claimed, or close the duplicate
    /// itself.
    /// @return The target.
    [[nodiscard]] static HandleTarget currentProcess() noexcept
    {
        return HandleTarget { Kind::CurrentProcess };
    }

    /// @return No process: grantHandle() refuses it. What a moved-from target is.
    [[nodiscard]] static HandleTarget none() noexcept { return HandleTarget { Kind::None }; }

    /// @return Whether it is none(): whether no handle can be granted to it.
    [[nodiscard]] bool isNone() const noexcept { return _kind == Kind::None; }

#ifdef _WIN32
    /// @param process A handle on the process, with PROCESS_DUP_HANDLE access; the target's own.
    /// @return That process.
    [[nodiscard]] static HandleTarget ofProcess(OwnedHandle process) noexcept;

    /// @return The handle a duplicate is made into the target through: this process's pseudo-handle for
    ///         currentProcess(); nothing for none().
    [[nodiscard]] std::optional<NativeHandle> process() const noexcept;
#endif

  private:
    /// Which process it names.
    enum class Kind : uint8_t
    {
        None,           ///< None: nothing is granted.
        CurrentProcess, ///< This process.
#ifdef _WIN32
        OtherProcess, ///< The process _process is a handle on.
#endif
    };

    explicit HandleTarget(Kind kind) noexcept: _kind { kind } {}

#ifdef _WIN32
    /// @param process A handle on another process.
    explicit HandleTarget(OwnedHandle process) noexcept;
#endif

    Kind _kind; ///< Which process it names.
#ifdef _WIN32
    OwnedHandle _process; ///< A handle on the target, for OtherProcess.
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
    Io, ///< The target is none(), or the system refused to duplicate it into the target (Windows).
};

/// Grants @p object to @p target. On Windows the duplicate has FILE_MAP_READ | FILE_MAP_WRITE access and is
/// the target's from now on, whether or not the PDU naming it is sent; on POSIX nothing happens before the
/// PDU is sent (PduStream::send(pdu, grant)).
/// @param target Who it is granted to.
/// @param object The handle; still the caller's.
/// @return The grant; `Io` if @p target is none(), or the system refused.
[[nodiscard]] std::expected<HandleGrant, GrantError> grantHandle(HandleTarget const& target,
                                                                 NativeHandle object);

class GrantedHandle;

/// Claims the handle a PDU just received from @p stream names by @p wireValue: on POSIX the descriptor that
/// arrived with it, which @p stream held; on Windows the value itself, unchecked but for its shape.
/// @param stream The helper's control stream, which received the PDU.
/// @param wireValue What the PDU names the handle by.
/// @return The claim; `Malformed` if no handle came with the PDU, or (POSIX) the value is not 0, or (Windows)
///         it names no handle this process could have: 0, not a multiple of 4, or a pseudo-handle.
[[nodiscard]] std::expected<GrantedHandle, StreamError> claimGrantedHandle(PduStream& stream,
                                                                           uint64_t wireValue);

/// A handle claimed off a stream, not yet known to be the object it was granted as. On POSIX a descriptor
/// this process was given, closed when the claim is destroyed. On Windows a value a peer chose, which may
/// name a handle of this process's own: let go, never closed, when the claim is destroyed, until adopt()
/// takes it as this process's once it proved to be the object. Move-only.
class GrantedHandle
{
  public:
    GrantedHandle(GrantedHandle&& other) noexcept: _handle { std::exchange(other._handle, NoNativeHandle) } {}
    GrantedHandle& operator=(GrantedHandle&& other) noexcept
    {
        if (this != &other)
        {
            letGo();
            _handle = std::exchange(other._handle, NoNativeHandle);
        }
        return *this;
    }
    GrantedHandle(GrantedHandle const&) = delete;
    GrantedHandle& operator=(GrantedHandle const&) = delete;
    ~GrantedHandle() { letGo(); }

    /// @return The handle, still the claim's: to map, or otherwise use, as the object it was granted as.
    [[nodiscard]] NativeHandle get() const noexcept { return _handle; }

    /// Takes the handle as this process's own, once using it proved it is the object it was granted as.
    /// @return The handle, closed when its owner is destroyed.
    [[nodiscard]] OwnedHandle adopt() && noexcept
    {
        return OwnedHandle { std::exchange(_handle, NoNativeHandle) };
    }

  private:
    friend std::expected<GrantedHandle, StreamError> claimGrantedHandle(PduStream& stream,
                                                                        uint64_t wireValue);

    /// @param handle What was claimed.
    explicit GrantedHandle(NativeHandle handle) noexcept: _handle { handle } {}

    /// Lets go of what it holds as this system does: closes a descriptor (POSIX), forgets a value (Windows).
    void letGo() noexcept;

    NativeHandle _handle = NoNativeHandle; ///< What was claimed; NoNativeHandle once adopted or moved from.
};

} // namespace vtextension::wasm::remote::transport
