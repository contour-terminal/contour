// SPDX-License-Identifier: Apache-2.0
#include <vtextension/wasm/wamr/WamrBackend.hpp>

#include <core/Utils.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <wasm_export.h>

#include <vtextension/wasm/wamr/WamrHandles.hpp>
#include <vtextension/wasm/wamr/WamrRuntime.hpp>
#include <vtextension/wasm/wamr/WamrThreadEnv.hpp>

#ifdef _WIN32
    #include <Windows.h>
#else
    #include <fcntl.h>
    #include <unistd.h>
#endif

namespace vtextension::wasm::wamr
{

namespace detail
{
    std::optional<int64_t> openNullDevice() noexcept
    {
#ifdef _WIN32
        auto* const handle = CreateFileW(L"NUL",
                                         GENERIC_READ | GENERIC_WRITE,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE,
                                         nullptr,
                                         OPEN_EXISTING,
                                         0,
                                         nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            return std::nullopt;
        return static_cast<int64_t>(reinterpret_cast<intptr_t>(handle));
#else
        auto const fd = ::open("/dev/null", O_RDWR | O_CLOEXEC);
        if (fd < 0)
            return std::nullopt;
        return fd;
#endif
    }

    void closeNullDevice(int64_t handle) noexcept
    {
#ifdef _WIN32
        CloseHandle(reinterpret_cast<HANDLE>(static_cast<intptr_t>(handle)));
#else
        ::close(static_cast<int>(handle));
#endif
    }
} // namespace detail

/// The host functions of one runtime, as registered with WAMR: one native-symbol array per import
/// module. WAMR keeps (and sorts) the very arrays, and the names they point to, until they are
/// unregistered, so each group owns its own and never moves them. What a module links to (trampoline,
/// signature, attachment) lives as long as the process (@see HostNative).
class HostRegistrations
{
  public:
    /// Registers @p hostFunctions with the running runtime.
    /// @return The registrations; `RuntimeUnavailable` if WAMR refused one, or there is no trampoline
    ///         for one (more than MaxHostFunctions, or one taking more than MaxHostParameters).
    [[nodiscard]] static std::expected<std::unique_ptr<HostRegistrations>, ExtensionError> create(
        std::span<HostFunction const> hostFunctions)
    {
        auto registrations = std::make_unique<HostRegistrations>();
        for (auto const [index, function]: core::views::enumerate(hostFunctions))
        {
            auto const native = hostNative(function.parameterCount, index);
            if (!native)
                return std::unexpected(ExtensionError::RuntimeUnavailable);
            registrations->groupOf(function.module)
                .entries.push_back(Entry { .name = std::string { function.name }, .native = *native });
        }
        auto const registered =
            std::ranges::all_of(registrations->_groups, [](std::unique_ptr<Group> const& group) {
                return group->publish().has_value();
            });
        if (!registered)
            return std::unexpected(ExtensionError::RuntimeUnavailable);
        return registrations;
    }

    HostRegistrations() = default;
    HostRegistrations(HostRegistrations const&) = delete;
    HostRegistrations(HostRegistrations&&) = delete;
    HostRegistrations& operator=(HostRegistrations const&) = delete;
    HostRegistrations& operator=(HostRegistrations&&) = delete;
    ~HostRegistrations() = default;

  private:
    /// One host function's name and what it is registered as.
    struct Entry
    {
        std::string name;  ///< The import's field name.
        HostNative native; ///< Its trampoline, signature and attachment.
    };

    /// The host functions of one import module.
    struct Group
    {
        std::string module;                ///< The import module name.
        std::vector<Entry> entries;        ///< Complete before publish(), never changed after.
        std::vector<NativeSymbol> symbols; ///< What WAMR holds while registered; points into entries.

        Group() = default;
        Group(Group const&) = delete;
        Group(Group&&) = delete;
        Group& operator=(Group const&) = delete;
        Group& operator=(Group&&) = delete;

        ~Group()
        {
            if (!symbols.empty())
                wasm_runtime_unregister_natives(module.c_str(), symbols.data());
        }

        /// Registers the group with WAMR.
        /// @return Nothing once WAMR holds it; `RuntimeUnavailable` if WAMR refused it.
        [[nodiscard]] std::expected<void, ExtensionError> publish()
        {
            auto table = std::vector<NativeSymbol> {};
            table.reserve(entries.size());
            for (auto const& entry: entries)
                table.push_back(NativeSymbol { .symbol = entry.name.c_str(),
                                               .func_ptr = entry.native.function,
                                               .signature = entry.native.signature,
                                               .attachment = entry.native.attachment });
            if (!wasm_runtime_register_natives_raw(
                    module.c_str(), table.data(), static_cast<uint32_t>(table.size())))
                return std::unexpected(ExtensionError::RuntimeUnavailable);
            symbols = std::move(table); // moving a vector keeps its buffer, which WAMR now holds
            return {};
        }
    };

    /// @return The group of @p module, added if there is none yet.
    [[nodiscard]] Group& groupOf(std::string_view module)
    {
        auto const existing = std::ranges::find_if(
            _groups, [&](std::unique_ptr<Group> const& group) { return group->module == module; });
        if (existing != _groups.end())
            return **existing;
        auto& group = *_groups.emplace_back(std::make_unique<Group>());
        group.module = std::string { module };
        return group;
    }

    std::vector<std::unique_ptr<Group>> _groups;
};

std::expected<std::shared_ptr<WamrRuntime>, ExtensionError> WamrRuntime::create(
    RuntimeLimits limits, std::span<HostFunction const> hostFunctions)
{
    if (auto acquired = acquireRuntime(); !acquired)
        return std::unexpected(acquired.error());

    auto const fail = [] {
        releaseRuntime();
        return std::unexpected(ExtensionError::RuntimeUnavailable);
    };

    detail::installMemoryGrowthHandler();
    auto hostRegistrations = HostRegistrations::create(hostFunctions);
    if (!hostRegistrations)
        return fail();
    auto const nullDevice = detail::openNullDevice();
    if (!nullDevice)
    {
        hostRegistrations->reset();
        return fail();
    }

    try
    {
        return std::make_shared<WamrRuntime>(
            PrivateTag {}, limits, *nullDevice, std::move(*hostRegistrations));
    }
    catch (...)
    {
        // The runtime's constructor did not complete, so what it was to be handed is released here,
        // the registrations while the runtime they are registered with is still up.
        hostRegistrations->reset();
        detail::closeNullDevice(*nullDevice);
        releaseRuntime();
        throw;
    }
}

WamrRuntime::WamrRuntime(PrivateTag /*unused*/,
                         RuntimeLimits limits,
                         int64_t nullDevice,
                         std::unique_ptr<HostRegistrations> hostRegistrations) noexcept:
    _limits { limits }, _nullDevice { nullDevice }, _hostRegistrations { std::move(hostRegistrations) }
{
}

WamrRuntime::~WamrRuntime()
{
    _hostRegistrations.reset();
    detail::closeNullDevice(_nullDevice);
    releaseRuntime();
}

std::expected<std::shared_ptr<IWasmModule>, ExtensionError> WamrRuntime::load(std::vector<std::byte> bytes)
{
    return loadModule(shared_from_this(), std::move(bytes));
}

namespace
{
    /// A thread's WAMR environment, held for as long as the guard lives.
    class WamrThreadGuard final: public IThreadGuard
    {
      public:
        explicit WamrThreadGuard(ThreadEnv threadEnv) noexcept: _threadEnv { std::move(threadEnv) } {}

        [[nodiscard]] uint64_t generation() const noexcept override { return _threadEnv.generation(); }

      private:
        ThreadEnv _threadEnv;
    };

    /// The facade over WAMR's process-wide state.
    class WamrBackend final: public IWasmBackend
    {
      public:
        [[nodiscard]] std::expected<std::shared_ptr<IWasmRuntime>, ExtensionError> createRuntime(
            RuntimeLimits limits, std::span<HostFunction const> hostFunctions) override
        {
            return WamrRuntime::create(limits, hostFunctions)
                .transform([](std::shared_ptr<WamrRuntime> runtime) -> std::shared_ptr<IWasmRuntime> {
                    return runtime;
                });
        }

        [[nodiscard]] std::expected<std::unique_ptr<IThreadGuard>, ExtensionError> enterThread() override
        {
            return ThreadEnv::enter().transform([](ThreadEnv threadEnv) -> std::unique_ptr<IThreadGuard> {
                return std::make_unique<WamrThreadGuard>(std::move(threadEnv));
            });
        }

        [[nodiscard]] bool isCurrentThreadPrepared() const override
        {
            return ThreadEnv::isCurrentThreadPrepared();
        }

        [[nodiscard]] uint64_t runtimeGeneration() const noexcept override
        {
            return wamr::runtimeGeneration();
        }

        [[nodiscard]] BackendCapabilities capabilities() const noexcept override
        {
            return BackendCapabilities { .memoryGrowth = MemoryGrowthReporting::Reported };
        }
    };
} // namespace

std::unique_ptr<IWasmBackend> makeWamrBackend()
{
    return std::make_unique<WamrBackend>();
}

} // namespace vtextension::wasm::wamr
