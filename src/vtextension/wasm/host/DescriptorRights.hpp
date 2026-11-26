// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What each descriptor the extension host keeps is used for once it is locked down, and so what a sandbox
/// that limits descriptor by descriptor (FreeBSD's Capsicum) leaves it. Named in the system's neutral words,
/// so that the table is the same everywhere and is tested everywhere; each such sandbox maps a right to its
/// own.

#include <cstdint>
#include <span>

namespace vtextension::wasm::host
{

/// What a descriptor the host keeps is for.
enum class DescriptorRole : uint8_t
{
    Control,       ///< The control channel: requests in, replies and granted handles over it.
    Lane,          ///< A lane channel, which carries the calls of one executor.
    Page,          ///< The interrupt page, mapped read-only before the lockdown.
    Wake,          ///< The wake signal's receiving end, which the interrupt listener waits on.
    StandardError, ///< Standard error, which the host tells why it fails on.
    EngineDevice,  ///< A device the engine opened before the lockdown: WAMR's null device, its guests' stdio.
    IdleStream,    ///< Standard input or output, which the host neither reads nor writes.
    StopReceiver,  ///< The wake receiver's stop pipe, the end its wait polls.
    StopSender,    ///< The wake receiver's stop pipe, the end stop() writes to.
    StopEvent, ///< The wake receiver's stop event, where one handle is both what its wait waits on and what
               ///< stop() signals (Windows).
};

/// One thing a descriptor may be used for. There is deliberately none for an ioctl: no descriptor needs one.
enum class DescriptorRight : uint8_t
{
    Read,           ///< read(2) and its kin.
    Write,          ///< write(2), writev(2) and their kin.
    Receive,        ///< recv(2), recvmsg(2): taking in what a socket carries, descriptors included.
    Send,           ///< send(2), sendmsg(2).
    Event,          ///< Waiting on it: poll(2), kevent(2).
    Shutdown,       ///< shutdown(2).
    Fstat,          ///< fstat(2): what C's stdio and WAMR's WASI ask of a stream.
    MapRead,        ///< Mapping it read-only, mmap(2).
    Seek,           ///< lseek(2): a guest's fd_seek on its null device.
    GetStatusFlags, ///< fcntl(2) F_GETFL and no other fcntl that needs a right: a guest's fd_fdstat_get.
};

/// The rights of one role.
struct DescriptorRoleRights
{
    DescriptorRole role;                     ///< The role.
    std::span<DescriptorRight const> rights; ///< What a descriptor of it may be used for; never empty.
};

/// @return Every DescriptorRole's rights, one row each, at its enumerator's index.
[[nodiscard]] std::span<DescriptorRoleRights const> descriptorRoleRights() noexcept;

/// @param role A role.
/// @return What a descriptor of @p role may be used for.
[[nodiscard]] std::span<DescriptorRight const> rightsOf(DescriptorRole role) noexcept;

} // namespace vtextension::wasm::host
