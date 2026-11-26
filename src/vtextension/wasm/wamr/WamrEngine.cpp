// SPDX-License-Identifier: Apache-2.0
#include <vtextension/wasm/wamr/WamrEngine.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <wasm_export.h>

#include <vtextension/wasm/wamr/WamrThreadEnv.hpp>

namespace vtextension::wasm::wamr
{

namespace
{
    /// The thread environment prepareThread() entered on the calling thread: kept until the thread ends,
    /// whose exit ends it there. WAMR's state is the process's, so one per thread serves every engine.
    thread_local std::optional<ThreadEnv> preparedThread;

    /// The text of each WamrEngineError.
    struct ErrorText
    {
        WamrEngineError error;
        std::string_view text;
    };

    constexpr auto ErrorTexts = std::array {
        ErrorText { WamrEngineError::CompilesNativeCode,
                    "its engine was built to compile code at run time, which this extension host's sandbox "
                    "forbids; it needs one that interprets" },
        ErrorText { WamrEngineError::RuntimeUnavailable, "its engine could not be initialized" },
        ErrorText { WamrEngineError::NullDeviceUnavailable, "the null device could not be opened" },
    };

    /// A WAMR backend holding WAMR's runtime up for as long as it lives.
    class WamrEngine final: public host::IHostedEngine
    {
        struct PrivateTag
        {
        };

      public:
        /// Takes a hold on WAMR's runtime for the engine.
        /// @param backend The backend it is a facade over.
        /// @return The engine; `RuntimeUnavailable` if WAMR could not initialize.
        /// @param devices The devices the backend keeps open: its shared null device.
        [[nodiscard]] static std::expected<std::unique_ptr<WamrEngine>, ExtensionError> create(
            std::unique_ptr<IWasmBackend> backend, std::vector<remote::transport::NativeHandle> devices)
        {
            if (auto acquired = acquireRuntime(); !acquired)
                return std::unexpected(acquired.error());
            try
            {
                return std::make_unique<WamrEngine>(PrivateTag {}, std::move(backend), std::move(devices));
            }
            catch (...)
            {
                releaseRuntime();
                throw;
            }
        }

        /// Use create(); public only so that std::make_unique can reach it. Takes over the hold create()
        /// took.
        WamrEngine(PrivateTag /*unused*/,
                   std::unique_ptr<IWasmBackend> backend,
                   std::vector<remote::transport::NativeHandle> devices) noexcept:
            _backend { std::move(backend) }, _devices { std::move(devices) }
        {
        }

        WamrEngine(WamrEngine const&) = delete;
        WamrEngine(WamrEngine&&) = delete;
        WamrEngine& operator=(WamrEngine const&) = delete;
        WamrEngine& operator=(WamrEngine&&) = delete;

        /// Releases the engine's hold on the runtime, after the backend.
        ~WamrEngine() override
        {
            _backend.reset();
            releaseRuntime();
        }

        [[nodiscard]] std::expected<std::shared_ptr<IWasmRuntime>, ExtensionError> createRuntime(
            RuntimeLimits limits, std::span<HostFunction const> hostFunctions) override
        {
            return _backend->createRuntime(limits, hostFunctions);
        }

        [[nodiscard]] std::expected<std::unique_ptr<IThreadGuard>, ExtensionError> enterThread() override
        {
            return _backend->enterThread();
        }

        [[nodiscard]] bool isCurrentThreadPrepared() const override
        {
            return _backend->isCurrentThreadPrepared();
        }

        [[nodiscard]] uint64_t runtimeGeneration() const noexcept override
        {
            return _backend->runtimeGeneration();
        }

        [[nodiscard]] BackendCapabilities capabilities() const noexcept override
        {
            return _backend->capabilities();
        }

        [[nodiscard]] remote::proto::MemoryMode memoryMode() const noexcept override
        {
            return remote::proto::MemoryMode::Copy;
        }

        [[nodiscard]] uint64_t memoryGuardBytes() const noexcept override { return 0; }

        /// The build's WAMR has the interpreter alone: no JIT and no AOT code
        /// (cmake/ContourThirdParties.cmake).
        [[nodiscard]] host::GuestCode guestCode() const noexcept override
        {
            return host::GuestCode::Interpreted;
        }

        /// The null device its runtimes bind their guests' standard streams to, opened as it was made.
        [[nodiscard]] std::vector<remote::transport::NativeHandle> heldDevices() const override
        {
            return _devices;
        }

        /// Enters WAMR's thread environment on the calling thread, unless it holds one of the running
        /// runtime already. The interpreter installs no signal handler, so this is all a thread needs.
        [[nodiscard]] std::expected<void, ExtensionError> prepareThread() override
        {
            if (preparedThread && preparedThread->generation() == wamr::runtimeGeneration())
                return {};
            preparedThread.reset();
            return ThreadEnv::enter().transform(
                [](ThreadEnv entered) { preparedThread.emplace(std::move(entered)); });
        }

      private:
        std::unique_ptr<IWasmBackend> _backend;
        std::vector<remote::transport::NativeHandle> _devices;
    };

    /// @return @p handle, a raw handle as a null device opener returns it, as the transport names one.
    [[nodiscard]] remote::transport::NativeHandle nativeHandleOf(int64_t handle) noexcept
    {
#ifdef _WIN32
        return reinterpret_cast<remote::transport::NativeHandle>(static_cast<intptr_t>(handle));
#else
        return static_cast<remote::transport::NativeHandle>(handle);
#endif
    }
} // namespace

NativeCompilation linkedNativeCompilation() noexcept
{
    auto const compiles =
        std::ranges::any_of(std::array { Mode_Fast_JIT, Mode_LLVM_JIT, Mode_Multi_Tier_JIT },
                            &wasm_runtime_is_running_mode_supported);
    return compiles ? NativeCompilation::Present : NativeCompilation::Absent;
}

std::string_view wamrEngineErrorText(WamrEngineError error) noexcept
{
    auto const row = std::ranges::find(ErrorTexts, error, &ErrorText::error);
    return row != ErrorTexts.end() ? row->text : std::string_view { "the engine could not be made" };
}

std::expected<std::unique_ptr<host::IHostedEngine>, WamrEngineError> makeWamrEngine(
    WamrEngineConfig const& config)
{
    // Checked before anything is opened: an engine that could compile would hold, or need, executable memory
    // that its host's sandbox, made for an interpreter, refuses.
    if (config.nativeCompilation() != NativeCompilation::Absent)
        return std::unexpected(WamrEngineError::CompilesNativeCode);
    auto devices = std::vector<remote::transport::NativeHandle> {};
    return NullDevice::openShared(config.openNullDevice)
        .transform_error([](ExtensionError) { return WamrEngineError::NullDeviceUnavailable; })
        .transform([&devices](NullDevice nullDevice) {
            if (auto const handle = nullDevice.sharedHandle())
                devices.push_back(nativeHandleOf(*handle));
            return makeWamrBackend(WamrConfig { .nullDevice = std::move(nullDevice) });
        })
        .and_then([&devices](std::unique_ptr<IWasmBackend> backend) {
            return WamrEngine::create(std::move(backend), std::move(devices))
                .transform([](std::unique_ptr<WamrEngine> engine) -> std::unique_ptr<host::IHostedEngine> {
                    return engine;
                })
                .transform_error([](ExtensionError) { return WamrEngineError::RuntimeUnavailable; });
        });
}

} // namespace vtextension::wasm::wamr
