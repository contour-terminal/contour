// SPDX-License-Identifier: Apache-2.0
#include <vtextension/wasm/wasmtime/WasmtimeMemoryCreator.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

#include <wasmtime.h>

#include <vtextension/wasm/host/MemoryProvision.hpp>
#include <vtextension/wasm/remote/proto/Pdu.hpp>

namespace vtextension::wasm::wasmtime
{

namespace
{
    /// What Wasmtime last asked the creator for on this thread.
    thread_local auto lastRequest = std::optional<testing::MemoryRequest> {};

    /// One linear memory: the provision it was made of, owned by Wasmtime from creation to its finalizer.
    struct ProvidedMemory
    {
        host::MemoryProvision provision; ///< The mapping it lives in, and how it asks to grow.
    };

    /// Wasmtime's LinearMemory::as_ptr, byte_size and byte_capacity in one: where the memory is, how much of
    /// it the guest spans, and the most it may span without moving.
    uint8_t* memoryOf(void* env, std::size_t* byteSize, std::size_t* byteCapacity) noexcept
    {
        auto const& mapping = static_cast<ProvidedMemory const*>(env)->provision.memory;
        // The mapping's sizes are within size_t: GuestMemoryMapping::map() refused any that is not.
        *byteSize = static_cast<std::size_t>(mapping.accessibleBytes());
        *byteCapacity = static_cast<std::size_t>(mapping.objectBytes());
        return reinterpret_cast<uint8_t*>(mapping.base());
    }

    /// Wasmtime's LinearMemory::grow_to: asks Contour, which opens the new bytes once it granted them. Any
    /// error makes the guest's memory.grow return -1.
    wasmtime_error_t* growMemory(void* env, std::size_t newBytes) noexcept
    {
        auto& provision = static_cast<ProvidedMemory*>(env)->provision;
        if (newBytes <= provision.memory.accessibleBytes())
            return nullptr;
        if (newBytes > provision.memory.objectBytes())
            return wasmtime_error_new("guest memory cannot grow past its object");
        // The request opens what Contour granted, and fails the host itself if it cannot.
        if (!provision.grow
            || provision.grow(provision.memory, newBytes) != remote::proto::GrowStatus::Granted)
            return wasmtime_error_new("guest memory growth was refused");
        return nullptr;
    }

    /// Ends a memory: unmaps its object.
    void finalizeMemory(void* env) noexcept
    {
        std::unique_ptr<ProvidedMemory> { static_cast<ProvidedMemory*>(env) }.reset();
    }

    /// Wasmtime's MemoryCreator::new_memory: the thread's provision, if it is what Wasmtime asks for.
    wasmtime_error_t* createMemory(void* /*env*/,
                                   wasm_memorytype_t const* /*type*/,
                                   std::size_t minimum,
                                   std::size_t /*maximum*/,
                                   std::size_t reservedBytes,
                                   std::size_t guardBytes,
                                   wasmtime_linear_memory_t* memory) noexcept
    {
        lastRequest =
            testing::MemoryRequest { .minimum = minimum, .reserved = reservedBytes, .guard = guardBytes };
        auto provision = host::takeProvision();
        if (!provision)
            return wasmtime_error_new("no guest memory was provided for this memory");
        auto const& mapping = provision->memory;
        if (minimum != mapping.accessibleBytes() || reservedBytes != mapping.objectBytes()
            || guardBytes > mapping.guardBytes())
            return wasmtime_error_new("the guest memory provided is not the memory asked for");
        auto owned = std::make_unique<ProvidedMemory>(ProvidedMemory { .provision = std::move(*provision) });
        *memory = wasmtime_linear_memory_t { .env = owned.release(),
                                             .get_memory = &memoryOf,
                                             .grow_memory = &growMemory,
                                             .finalizer = &finalizeMemory };
        return nullptr;
    }
} // namespace

void provideMemories(wasm_config_t* config) noexcept
{
    auto creator =
        wasmtime_memory_creator_t { .env = nullptr, .new_memory = &createMemory, .finalizer = nullptr };
    wasmtime_config_host_memory_creator_set(config, &creator);
}

namespace testing
{
    std::optional<MemoryRequest> lastMemoryRequest() noexcept
    {
        return lastRequest;
    }
} // namespace testing

} // namespace vtextension::wasm::wasmtime
