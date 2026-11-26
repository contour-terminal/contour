// SPDX-License-Identifier: Apache-2.0
#include <vtextension/wasm/host/ContainerSandbox.hpp>

#include <optional>
#include <utility>

namespace vtextension::wasm::host
{

namespace
{
    using remote::proto::LockdownState;
    using remote::proto::LockdownStep;

    class ContainerSandbox final: public ISandbox
    {
      public:
        explicit ContainerSandbox(ContainerApi api): _api { std::move(api) } {}

        [[nodiscard]] std::expected<void, ConfinementFailure> restrictBeforeThreads() override { return {}; }

        [[nodiscard]] std::expected<void, ConfinementFailure> lockDown() override
        {
            auto const container = _api.tokenContainer();
            if (!container)
                return std::unexpected(
                    ConfinementFailure { .step = LockdownStep::Container, .code = container.error() });
            if (*container == TokenContainer::None)
                return {};

            // Every step runs; the first that failed is the one stated.
            auto first = std::optional<ConfinementFailure> {};
            auto const note = [&first](LockdownStep step, int code) {
                if (!first)
                    first = ConfinementFailure { .step = step, .code = code };
            };
            for (auto const& mitigation: _api.mitigations)
                if (auto const set = mitigation(); !set)
                    note(LockdownStep::Mitigations, set.error());
            if (auto const job = _api.jobMembership(); !job)
                note(LockdownStep::Job, job.error());
            else if (*job == JobMembership::Outside)
                note(LockdownStep::Job, NotInJobCode);
            if (first)
                return std::unexpected(*first);
            _state = LockdownState::LockedDown;
            return {};
        }

        /// @return `LockedDown` once lockDown() verified the container and the job and set every mitigation;
        ///         `Unsandboxed` before, and when the host runs in no container.
        [[nodiscard]] LockdownState lockdownState() const noexcept override { return _state; }

      private:
        ContainerApi _api;
        LockdownState _state = LockdownState::Unsandboxed;
    };
} // namespace

std::unique_ptr<ISandbox> makeContainerSandbox(ContainerApi api)
{
    return std::make_unique<ContainerSandbox>(std::move(api));
}

} // namespace vtextension::wasm::host
