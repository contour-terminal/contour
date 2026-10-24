// SPDX-License-Identifier: Apache-2.0
#include <vthost/control/LoopControlBridge.hpp>

#include <core/Assert.hpp>

#include <future>
#include <memory>
#include <utility>

#include <vtcontrol/Dispatch.hpp>

namespace vthost::control
{

namespace
{
    using Reply = std::expected<std::string, vtcontrol::ControlError>;

    [[nodiscard]] std::unexpected<vtcontrol::ControlError> failure(vtcontrol::ControlErrorCode code,
                                                                   std::string message)
    {
        return std::unexpected(
            vtcontrol::ControlError { .code = code, .message = std::move(message), .candidates = {} });
    }

    /// A sink that drops what it is given. An extension subscribes to nothing -- `events.subscribe` is
    /// `SocketOnly` -- so nothing is ever delivered to it.
    class DroppingEventSink final: public vtcontrol::EventSink
    {
      public:
        void deliverEvent(uint32_t /*subscription*/, vtcontrol::Event const& /*event*/) override {}
    };

    /// What a handler knows of an extension's call: its pane, and what it is granted.
    class ExtensionCallContext final: public vtcontrol::CallContext
    {
      public:
        ExtensionCallContext(std::string pane, vtcontrol::ControlGrants granted):
            _pane { std::move(pane) }, _granted { granted }
        {
        }

        [[nodiscard]] std::string_view caller() const noexcept override { return _pane; }
        [[nodiscard]] vtcontrol::EventSink& events() noexcept override { return _sink; }
        [[nodiscard]] vtcontrol::ControlGrants grants() const noexcept override { return _granted; }

      private:
        std::string _pane;
        vtcontrol::ControlGrants _granted;
        DroppingEventSink _sink;
    };
} // namespace

LoopControlBridge::LoopControlBridge(core::net::EventLoop& loop, ExtensionCallerResolver resolve):
    _loop { &loop }, _resolve { std::move(resolve) }
{
}

std::expected<std::string, vtcontrol::ControlError> LoopControlBridge::call(
    std::string_view method,
    std::string_view params,
    vtcontrol::ControlGrants granted,
    std::chrono::milliseconds timeout) const
{
    // On the loop's thread, the reply could only come after this returned: the call would wait for itself.
    Require(!_loop->isOnWorkerThread());

    // The reply's slot, owned by the job and by this call alike: whichever lets go last releases it. A reply
    // written after this call stopped waiting lands here and goes nowhere else.
    auto slot = std::make_shared<std::promise<Reply>>();
    auto reply = slot->get_future();
    _loop->post([resolve = _resolve,
                 slot,
                 method = std::string { method },
                 params = std::string { params },
                 granted]() {
        auto const caller = resolve();
        if (!caller)
        {
            slot->set_value(failure(vtcontrol::ControlErrorCode::Unsupported,
                                    "the extension's pane is no longer hosted"));
            return;
        }
        // A handler may answer later; the context lives as long as the completion that answers.
        auto context = std::make_shared<ExtensionCallContext>(caller->pane, granted);
        auto& callContext = *context;
        vtcontrol::dispatch(caller->api,
                            callContext,
                            vtcontrol::Caller::Extension,
                            method,
                            params,
                            [slot, context](Reply result) { slot->set_value(std::move(result)); });
    });

    if (reply.wait_for(timeout) != std::future_status::ready)
        return failure(vtcontrol::ControlErrorCode::Failed, "timed out");
    try
    {
        return reply.get();
    }
    catch (std::future_error const&)
    {
        // The job was destroyed unrun: the loop went first.
        return failure(vtcontrol::ControlErrorCode::Unsupported, "the daemon is shutting down");
    }
}

} // namespace vthost::control
