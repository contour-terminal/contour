// SPDX-License-Identifier: Apache-2.0
#include <vtextension/GuestCall.hpp>

#include <core/Assert.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>

namespace vtextension::detail
{

namespace
{
    /// The bytes of one WebAssembly page.
    constexpr auto WasmPageBytes = uint64_t { 64 } * 1024;
} // namespace

/// The guest's linear memory as the codec reaches it: every view is bounds-checked by the runtime,
/// allocations go through the guest's `cabi_realloc` and are checked as well, and what lifting may
/// spend on the host is bounded per host call.
class GuestCall::GuestMemory final: public abi::IGuestMemory
{
  public:
    explicit GuestMemory(GuestCall& call) noexcept: _call { call } {}

    /// Gives the next lift the full budget of one host call.
    void resetLiftBudget() noexcept { _liftBudget = _call._limits.maxHostCallBytes; }

    [[nodiscard]] abi::Result<std::span<std::byte>> view(abi::Address address, uint32_t size) override
    {
        auto const bytes = _call._instance->memory(address, size);
        if (!bytes)
            return std::unexpected(ExtensionError::Trapped);
        return *bytes;
    }

    [[nodiscard]] abi::Result<abi::Address> allocate(uint32_t size, uint32_t alignment) override
    {
        auto argv = std::array<uint32_t, 4> { 0, 0, alignment, size }; // old pointer, old size, align, size
        if (auto called = _call.callExport(_call._realloc, argv); !called)
            return std::unexpected(called.error());
        // The guest's allocator is the guest's: what it answers is checked like any other address.
        auto const address = argv[0];
        if (alignment == 0 || address % alignment != 0)
            return std::unexpected(ExtensionError::Trapped);
        return view(address, size).transform([&](std::span<std::byte>) { return address; });
    }

    [[nodiscard]] abi::Result<void> chargeLift(uint64_t bytes) override
    {
        if (bytes > _liftBudget)
            return std::unexpected(ExtensionError::Trapped);
        _liftBudget -= bytes;
        return {};
    }

  private:
    GuestCall& _call;
    uint64_t _liftBudget = 0; ///< What lifting may still spend in the current host call.
};

GuestCall::GuestCall(wasm::IWasmBackend& backend,
                     core::platform::IClock& clock,
                     IDeadlineWatchdog& watchdog,
                     ExtensionLimits const& limits):
    _backend { backend },
    _clock { clock },
    _watchdog { watchdog },
    _limits { limits },
    _memory { std::make_unique<GuestMemory>(*this) }
{
}

GuestCall::~GuestCall() = default;

std::expected<void, ExtensionError> GuestCall::instantiate(wasm::IWasmModule& module,
                                                           wasm::IHostContext& context)
{
    Require(_instance == nullptr);
    auto const exceedsCap = [&](wasm::ExportDescription const& entry) {
        return entry.kind == wasm::ExternKind::Memory
               && uint64_t { entry.initialPages } * WasmPageBytes > _limits.memoryBytes;
    };
    if (std::ranges::any_of(module.exports(), exceedsCap))
        return std::unexpected(ExtensionError::OutOfMemory);

    auto instance = module.instantiate(context);
    if (!instance)
        return std::unexpected(instance.error());
    _instance = std::move(*instance);

    // The world check found it; a runtime that cannot is one that cannot instantiate the module.
    auto const realloc = _instance->function("cabi_realloc");
    if (!realloc)
        return std::unexpected(ExtensionError::InvalidComponent);
    _realloc = *realloc;
    return {};
}

std::optional<wasm::ExportFunction> GuestCall::function(std::string_view name) const
{
    return _instance->function(name);
}

std::expected<void, ExtensionError> GuestCall::initialize()
{
    // Not kept with the owner's exports: nothing but this may call it.
    auto const initialize = function("_initialize");
    if (!initialize)
        return {};
    return guarded(_limits.loadDeadline, [&] { return callExport(*initialize, {}); });
}

std::expected<void, ExtensionError> GuestCall::beginCall(std::chrono::milliseconds timeout)
{
    Require(_backend.isCurrentThreadPrepared());
    if (_fault)
        return std::unexpected(*_fault);
    _hostCallError.reset();
    resetLiftBudget();
    _deadline = _clock.now() + timeout;
    return {};
}

std::optional<ExtensionError> GuestCall::endCall(std::optional<ExtensionError> error) noexcept
{
    if (error)
        _fault = error;
    else if (isDeadlineExpired())
        // The deadline expired after the guest returned, while the host was still lifting the result:
        // the call overran all the same, and the instance is interrupted.
        _fault = ExtensionError::DeadlineExceeded;
    return _fault;
}

void GuestCall::resetLiftBudget() noexcept
{
    _memory->resetLiftBudget();
}

abi::IGuestMemory& GuestCall::memory() noexcept
{
    return *_memory;
}

GuestCall::ResumeDeadlineOnExit::~ResumeDeadlineOnExit()
{
    _call._deadline = _call._clock.now() + _remaining;
    _call._watchdog.armDeadline(_call.watchToken(), std::chrono::ceil<std::chrono::milliseconds>(_remaining));
}

WatchToken GuestCall::watchToken() noexcept
{
    return WatchToken { .instance = _instance.get(), .state = &_deadlineState };
}

bool GuestCall::isDeadlineExpired() const noexcept
{
    return _deadlineState.load(std::memory_order_acquire) == DeadlineState::Expired;
}

std::expected<void, ExtensionError> GuestCall::callExport(wasm::ExportFunction function,
                                                          std::span<uint32_t> argv)
{
    return _instance->call(function, argv).transform_error([this](wasm::CallFailure failure) {
        return guestFailure(failure);
    });
}

ExtensionError GuestCall::guestFailure(wasm::CallFailure failure) const noexcept
{
    if (_hostCallError)
        return *_hostCallError;
    if (isDeadlineExpired())
        return ExtensionError::DeadlineExceeded;
    // Refused growth is seen only as the reason this very call failed. Growth refused in a call that
    // returned (a guest allocator that copes with `memory.grow` failing) is not reported by the runtime
    // interface, so a later failure in the same guarded body is `Trapped`, where WAMR driven directly
    // once said `OutOfMemory` -- a documented deviation, in the label only. Refusal inside a host call's
    // allocation does reach here as `OutOfMemory`: the host call records it as its error, which ranks first.
    if (failure == wasm::CallFailure::MemoryGrowthRefused)
        return ExtensionError::OutOfMemory;
    return ExtensionError::Trapped;
}

std::unexpected<ExtensionError> GuestCall::failHostCall(ExtensionError error) noexcept
{
    if (!_hostCallError)
        _hostCallError = error;
    return std::unexpected(error);
}

} // namespace vtextension::detail
