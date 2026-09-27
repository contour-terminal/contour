// SPDX-License-Identifier: Apache-2.0
#include <core/net/windows/WaitCompletionPacket.hpp>

#include <cstdint>

#include <windows.h>

namespace core::net::detail
{

namespace
{
    /// The `NTSTATUS` value every one of these calls reports success with. Spelled here
    /// rather than included: `STATUS_SUCCESS` lives in `ntstatus.h`, which redefines a
    /// long list of macros `windows.h` has already defined and warns on every one.
    constexpr auto StatusSuccess = std::int32_t { 0 };

    using NtCreateWaitCompletionPacketFn = std::int32_t(__stdcall*)(void** packet,
                                                                    std::uint32_t desiredAccess,
                                                                    void* objectAttributes);

    using NtAssociateWaitCompletionPacketFn = std::int32_t(__stdcall*)(void* packet,
                                                                       void* completionPort,
                                                                       void* target,
                                                                       void* keyContext,
                                                                       void* apcContext,
                                                                       std::int32_t ioStatus,
                                                                       std::uintptr_t ioStatusInformation,
                                                                       unsigned char* alreadySignalled);

    using NtCancelWaitCompletionPacketFn = std::int32_t(__stdcall*)(void* packet,
                                                                    unsigned char removeSignalledPacket);

    /// The three entry points, resolved once.
    ///
    /// All three or none: a build that exported two of them and not the third would let
    /// this arm a wait it could not take back, and an association that cannot be
    /// cancelled is a packet delivered into a port after the object it names is gone.
    struct Entries
    {
        NtCreateWaitCompletionPacketFn create = nullptr;
        NtAssociateWaitCompletionPacketFn associate = nullptr;
        NtCancelWaitCompletionPacketFn cancel = nullptr;

        /// @return Whether every entry point was found.
        [[nodiscard]] bool complete() const noexcept
        {
            return create != nullptr && associate != nullptr && cancel != nullptr;
        }
    };

    /// Resolves the entry points from the already-loaded `ntdll`.
    ///
    /// `GetModuleHandleW`, never `LoadLibrary`: every Win32 process has `ntdll` mapped
    /// before `main`, so this borrows it and takes no reference that would have to be
    /// released. And never a link against `ntdll.lib` — core-cpp is a library inside
    /// other people's builds, and adding an undocumented import to every consumer's
    /// executable to save a helper thread is not a trade this is allowed to make for
    /// them (`.agent/rules/library-hygiene.md`).
    /// @return What was found; @c Entries::complete() says whether that is usable.
    [[nodiscard]] Entries resolveEntries() noexcept
    {
        auto entries = Entries {};
        auto* const ntdll = GetModuleHandleW(L"ntdll.dll");
        if (ntdll == nullptr)
            return entries;

        // reinterpret_cast straight from FARPROC, never through `void*`: a function
        // pointer laundered through an object pointer is a conversion the standard does
        // not define, and it is the one spelling clang-tidy refuses outright
        // (bugprone-casting-through-void). This is the documented way to use
        // GetProcAddress and there is no better one.
        entries.create = reinterpret_cast<NtCreateWaitCompletionPacketFn>(
            GetProcAddress(ntdll, "NtCreateWaitCompletionPacket"));
        entries.associate = reinterpret_cast<NtAssociateWaitCompletionPacketFn>(
            GetProcAddress(ntdll, "NtAssociateWaitCompletionPacket"));
        entries.cancel = reinterpret_cast<NtCancelWaitCompletionPacketFn>(
            GetProcAddress(ntdll, "NtCancelWaitCompletionPacket"));
        if (!entries.complete())
            return Entries {};
        return entries;
    }

    /// @return The resolved entry points. Resolved on the first call; the answer cannot
    ///         change afterwards, because a module already mapped is not unmapped.
    [[nodiscard]] Entries const& entries() noexcept
    {
        static auto const resolved = resolveEntries();
        return resolved;
    }

    /// What `NtCreateWaitCompletionPacket` is asked for. `GENERIC_ALL` on the object,
    /// which is what every published use of this call passes; the object is
    /// process-private and unnamed, so there is nothing narrower to ask for.
    constexpr auto PacketAccess = std::uint32_t { GENERIC_ALL };
} // namespace

bool waitCompletionPacketsAvailable() noexcept
{
    return entries().complete();
}

WaitCompletionPacket::WaitCompletionPacket() noexcept
{
    auto const& api = entries();
    if (!api.complete())
        return;
    auto* handle = static_cast<void*>(nullptr);
    if (api.create(&handle, PacketAccess, nullptr) == StatusSuccess)
        _handle = handle;
}

WaitCompletionPacket::~WaitCompletionPacket()
{
    if (_handle != nullptr)
        CloseHandle(_handle);
}

bool WaitCompletionPacket::associate(void* port, void* target, std::uintptr_t key, void* context) noexcept
{
    // `complete()` as well as the handle: a handle can only exist if the entry points
    // were found, so this is already true — but it is true by an invariant three
    // functions away, and stating it here is what makes the call below locally provably
    // not through a null pointer.
    if (_handle == nullptr || port == nullptr || target == nullptr || !entries().complete())
        return false;
    // `alreadySignalled` is not read: a target that is already signalled has its packet
    // queued there and then, which is the level-triggered behaviour wanted, and the
    // caller learns about it by dequeuing the packet like any other. Passing nullptr is
    // allowed and says the same thing with less to go wrong.
    return entries().associate(_handle,
                               port,
                               target,
                               reinterpret_cast<void*>(key),
                               context,
                               StatusSuccess,
                               /*ioStatusInformation*/ 0,
                               /*alreadySignalled*/ nullptr)
           == StatusSuccess;
}

bool WaitCompletionPacket::cancel() noexcept
{
    if (_handle == nullptr || !entries().complete())
        return true; // nothing was ever armed through this, so nothing is outstanding

    // `removeSignalledPacket` = TRUE: take the queued packet out of the port too, not
    // merely the association. With FALSE the call answers STATUS_PENDING for a packet
    // already queued and leaves it there, which is the state this cannot report —
    // `true` here must mean "the port holds nothing of mine", and only the removing
    // form can establish that.
    //
    // Anything but STATUS_SUCCESS is read as "a packet is or was in flight", which is
    // the safe direction: it costs a share held until the dequeue releases it, where
    // the other reading costs a share released twice.
    return entries().cancel(_handle, /*removeSignalledPacket*/ TRUE) == StatusSuccess;
}

} // namespace core::net::detail
