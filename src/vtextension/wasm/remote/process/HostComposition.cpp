// SPDX-License-Identifier: Apache-2.0
#include <vtextension/wasm/remote/process/HostComposition.hpp>

#include <core/Clock.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <utility>

#include <vtextension/ExtensionError.hpp>
#include <vtextension/wasm/remote/process/ProcessHostConnector.hpp>

namespace vtextension::wasm::remote::process
{

namespace
{
    /// A reason a supervisor could not start, and why no host serves when it cannot.
    struct SupervisorRefusalRow
    {
        SupervisorError error;       ///< Why it could not start.
        remote::HostRefusal refusal; ///< What is logged for it.
    };

    constexpr auto SupervisorRefusals = std::array {
        // The launcher made no set to wait on: on Linux, a kernel without pidfds.
        SupervisorRefusalRow { .error = SupervisorError::WaitSetUnavailable,
                               .refusal = remote::HostRefusal::Unsupported },
        SupervisorRefusalRow { .error = SupervisorError::ThreadUnavailable,
                               .refusal = remote::HostRefusal::OutOfResources },
    };

    /// @return Why no host serves when a supervisor fails to start for @p error.
    [[nodiscard]] constexpr remote::HostRefusal refusalOf(SupervisorError error) noexcept
    {
        auto const row = std::ranges::find(SupervisorRefusals, error, &SupervisorRefusalRow::error);
        return row != SupervisorRefusals.end() ? row->refusal : remote::HostRefusal::LaunchRefused;
    }

    /// The connector of a composition that cannot start a host: every connect() fails `HostUnavailable`
    /// without launching anything, and the first says why.
    class UnavailableHostConnector final: public remote::IHostConnector
    {
      public:
        /// @param refusal Why no host can be started.
        /// @param refusals Where that is logged, once.
        UnavailableHostConnector(remote::HostRefusal refusal, remote::HostRefusalLog::Writer refusals):
            _refusal { refusal }, _refusals { std::move(refusals) }
        {
        }

        [[nodiscard]] std::expected<remote::HostConnection, ExtensionError> connect() override
        {
            _refusals.refused(_refusal);
            return std::unexpected(ExtensionError::HostUnavailable);
        }

        /// Never called: no host of it ever reached a handshake.
        void refused(remote::HostRefusal /*refusal*/) override {}

      private:
        remote::HostRefusal _refusal;
        remote::HostRefusalLog _refusals;
    };

    /// A supervisor, or why no host can be started.
    using StartedSupervisor = std::expected<std::unique_ptr<ExtensionHostSupervisor>, remote::HostRefusal>;

    /// @return The supervisor @p config asks for, unless there is no helper to supervise.
    [[nodiscard]] StartedSupervisor startSupervisor(HostCompositionConfig const& config)
    {
        if (!config.helper)
            return std::unexpected(remote::HostRefusal::HelperNotFound);
        return ExtensionHostSupervisor::start(SupervisorConfig { .launcher = config.launcher,
                                                                 .clock = config.clock,
                                                                 .teardownGrace = config.teardownGrace })
            .transform_error(&refusalOf);
    }

    /// @return The connector starting @p config's helper through @p supervisor; one that starts none, and
    ///         says why, if there is no supervisor.
    [[nodiscard]] std::unique_ptr<remote::IHostConnector> connectorOver(StartedSupervisor const& supervisor,
                                                                        HostCompositionConfig const& config)
    {
        if (!supervisor)
            return std::make_unique<UnavailableHostConnector>(supervisor.error(), config.refusals);
        return std::make_unique<ProcessHostConnector>((*supervisor)->mailbox(),
                                                      ProcessHostConfig {
                                                          .helper = *config.helper,
                                                          .environment = defaultHelperEnvironment(),
                                                          .extraArguments = {},
                                                          .caps = {},
                                                          .standardError = defaultHelperStandardError(),
                                                      },
                                                      config.refusals);
    }
} // namespace

std::shared_ptr<HostComposition> HostComposition::create(HostCompositionConfig const& config)
{
    auto supervisor = startSupervisor(config);
    auto connector = connectorOver(supervisor, config);
    return std::make_shared<HostComposition>(
        Passkey {}, config.teardownGrace, std::move(supervisor).value_or(nullptr), std::move(connector));
}

HostComposition::HostComposition(Passkey,
                                 std::chrono::milliseconds teardownGrace,
                                 std::unique_ptr<ExtensionHostSupervisor> supervisor,
                                 std::unique_ptr<remote::IHostConnector> connector):
    _teardownGrace { teardownGrace },
    _kind { makeRemoteKindState() },
    _supervisor { std::move(supervisor) },
    _connector { std::move(connector) }
{
}

HostComposition::~HostComposition() = default;

std::unique_ptr<IWasmBackend> HostComposition::backend(BackendConfig config)
{
    if (!config.connector)
        // Shares the composition's ownership: the composition lives while anything can still connect.
        config.connector = std::shared_ptr<remote::IHostConnector> { shared_from_this(), _connector.get() };
    return makeRemoteBackend(_kind, std::move(config));
}

std::shared_ptr<HostComposition> sharedHostComposition(std::filesystem::path helper,
                                                       LockdownRequirement lockdown,
                                                       remote::HostRefusalLog::Writer refusals)
{
    // The composition of this process for each lockdown requirement while a backend holds it, and the helper
    // it was made for: a refusal one requirement keeps for good -- a host running unconfined -- must not stop
    // the backends of the other. One slot each, as a process finds its one helper beside itself, the same
    // path every time. Weak, so that a composition, and its supervisor's thread, end with the last backend
    // rather than at exit.
    struct Slot
    {
        std::filesystem::path helper;
        std::weak_ptr<HostComposition> composition;
    };
    static auto mutex = std::mutex {};
    static auto slots = std::array<Slot, std::size_t { std::to_underlying(LastLockdownRequirement) } + 1> {};

    auto const lock = std::scoped_lock { mutex };
    auto& slot = slots.at(std::to_underlying(lockdown));
    if (auto composition = slot.composition.lock(); composition && slot.helper == helper)
        return composition;
    auto composition = HostComposition::create(HostCompositionConfig {
        .launcher = makeSystemProcessLauncher(),
        .clock = &core::defaultSteadyClock(),
        .helper = helper,
        .refusals = std::move(refusals),
        .teardownGrace = teardownGraceFor(ThisBuildInstrumentation),
    });
    slot.helper = std::move(helper);
    slot.composition = composition;
    return composition;
}

std::unique_ptr<IWasmBackend> makeProcessBackend(
    BackendConfig config,
    std::expected<std::filesystem::path, HelperLocationError> helper,
    remote::HostRefusalLog::Writer refusals)
{
    if (helper)
        return sharedHostComposition(*std::move(helper), config.lockdown, std::move(refusals))
            ->backend(std::move(config));
    // No helper to start: every load fails `HostUnavailable`, and the first says why.
    return HostComposition::create(HostCompositionConfig {
                                       .launcher = makeSystemProcessLauncher(),
                                       .clock = &core::defaultSteadyClock(),
                                       .helper = std::move(helper),
                                       .refusals = std::move(refusals),
                                       .teardownGrace = teardownGraceFor(ThisBuildInstrumentation),
                                   })
        ->backend(std::move(config));
}

} // namespace vtextension::wasm::remote::process
