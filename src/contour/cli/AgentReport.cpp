// SPDX-License-Identifier: Apache-2.0
#include <contour/cli/AgentReport.hpp>

#include <array>
#include <optional>
#include <span>
#include <string>
#include <utility>

// clang-format off
#ifdef _WIN32
    #include <windows.h>
#else
    #include <cerrno>
    #include <poll.h>
    #include <unistd.h>
#endif
// clang-format on

#include <vtcontrol/Api.hpp>
#include <vtcontrol/Json.hpp>
#include <vthost/client/ControlClient.hpp>

namespace contour::cli
{

namespace
{
    /// The wire request for what a hook meant.
    struct Request
    {
        std::string_view method;
        std::string params;
    };

    [[nodiscard]] std::optional<Request> requestFor(HookOutcome const& outcome,
                                                    std::string_view integrationId,
                                                    std::string_view pane)
    {
        if (auto const* report = std::get_if<StatusReport>(&outcome))
            return Request { "pane.reportStatus",
                             vtcontrol::encodeJson(vtcontrol::ReportStatusRequest {
                                 .pane = std::string { pane },
                                 .integration = std::string { integrationId },
                                 .state = report->state,
                                 .kind = report->kind,
                                 .title = {},
                                 .message = report->message,
                                 .progress = std::nullopt }) };
        if (std::holds_alternative<StatusClear>(outcome))
            return Request { "pane.clearStatus",
                             vtcontrol::encodeJson(vtcontrol::ClearStatusRequest {
                                 .pane = std::string { pane },
                                 .integration = std::string { integrationId } }) };
        return std::nullopt;
    }

    /// The bytes one read takes at most.
    constexpr auto ReadChunkBytes = std::size_t { 64 } * 1024;

    /// @return What is left of the time until @p deadline on @p clock, in whole milliseconds rounded up; zero
    ///         once it has passed.
    [[nodiscard]] std::chrono::milliseconds timeLeft(core::platform::IClock const& clock,
                                                     core::platform::SteadyTimePoint deadline)
    {
        auto const left = deadline - clock.now();
        if (left <= core::platform::SteadyDuration::zero())
            return std::chrono::milliseconds::zero();
        return std::chrono::ceil<std::chrono::milliseconds>(left);
    }

    /// Reads a handle in chunks, each waited for no longer than the deadline leaves.
    class HandlePayloadReader final: public PayloadReader
    {
      public:
        HandlePayloadReader(core::platform::NativeHandle input, core::platform::IClock const& clock):
            _input { input }, _clock { clock }
        {
        }

        [[nodiscard]] std::optional<std::string> read(std::size_t maxBytes,
                                                      core::platform::SteadyTimePoint deadline) override
        {
            auto payload = std::string {};
            auto chunk = std::array<char, ReadChunkBytes> {};
            while (true)
            {
                auto const got = readSome(chunk, deadline);
                if (!got)
                    return std::nullopt;
                if (*got == 0)
                    return payload;
                if (payload.size() + *got > maxBytes)
                    return std::nullopt;
                payload.append(chunk.data(), *got);
            }
        }

      private:
        /// Waits until @p deadline at most for input, and takes what there is.
        /// @return The bytes taken, 0 at the end of the input; nothing on an error, or when the deadline
        ///         passed with none.
        [[nodiscard]] std::optional<std::size_t> readSome(std::span<char> buffer,
                                                          core::platform::SteadyTimePoint deadline)
        {
#ifdef _WIN32
            // A pipe -- what an agent hands its hook -- cannot be waited on: it is peeked at until its bytes
            // come, which a hook's few kilobytes do at once. Anything else is read as it is.
            if (GetFileType(_input) == FILE_TYPE_PIPE)
            {
                auto available = DWORD {};
                while (PeekNamedPipe(_input, nullptr, 0, nullptr, &available, nullptr) != 0 && available == 0)
                {
                    if (timeLeft(_clock, deadline) == std::chrono::milliseconds::zero())
                        return std::nullopt;
                    Sleep(PipePollMilliseconds);
                }
            }
            auto taken = DWORD {};
            if (ReadFile(_input, buffer.data(), static_cast<DWORD>(buffer.size()), &taken, nullptr) == 0)
                return GetLastError() == ERROR_BROKEN_PIPE ? std::optional { std::size_t { 0 } }
                                                           : std::nullopt;
            return std::size_t { taken };
#else
            while (true)
            {
                auto const left = timeLeft(_clock, deadline);
                if (left == std::chrono::milliseconds::zero())
                    return std::nullopt;
                auto ready = pollfd { .fd = _input, .events = POLLIN, .revents = 0 };
                auto const polled = ::poll(&ready, 1, static_cast<int>(left.count()));
                if (polled < 0 && errno == EINTR)
                    continue;
                if (polled <= 0)
                    return std::nullopt;
                auto const taken = ::read(_input, buffer.data(), buffer.size());
                if (taken < 0 && errno == EINTR)
                    continue;
                if (taken < 0)
                    return std::nullopt;
                return static_cast<std::size_t>(taken);
            }
#endif
        }

#ifdef _WIN32
        /// How often a pipe with nothing in it yet is looked at again.
        static constexpr auto PipePollMilliseconds = DWORD { 5 };
#endif

        core::platform::NativeHandle _input;
        core::platform::IClock const& _clock;
    };
} // namespace

std::unique_ptr<PayloadReader> makeHandlePayloadReader(core::platform::NativeHandle input,
                                                       core::platform::IClock const& clock)
{
    return std::make_unique<HandlePayloadReader>(input, clock);
}

ChannelFactory makeDaemonChannelFactory(core::Environment const& env)
{
    return [&env]() -> std::unique_ptr<ControlChannel> {
        auto const transport = vthost::client::resolveControlEndpoint({}, env);
        if (!transport)
            return nullptr;
        return makeTransportChannel(*transport);
    };
}

int runAgentReport(AgentIntegration const& integration,
                   PayloadReader& payload,
                   core::Environment const& env,
                   ChannelFactory const& openChannel,
                   core::platform::IClock const& clock)
{
    auto const daemon = env.get("CONTOUR_DAEMON");
    auto const pane = env.get("CONTOUR_PANE");
    if (!daemon || daemon->empty() || !pane || pane->empty())
        return 0;

    auto const deadline = clock.now() + AgentReportTimeout;
    auto const text = payload.read(MaxAgentPayloadBytes, deadline);
    if (!text)
        return 0;
    auto document = glz::generic {};
    if (glz::read_json(document, *text))
        return 0;
    auto const request = requestFor(integration.map(document), integration.integrationId, *pane);
    if (!request)
        return 0;

    auto const left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock.now());
    if (left <= std::chrono::milliseconds::zero())
        return 0;
    auto const channel = openChannel();
    if (channel == nullptr)
        return 0;
    // The daemon's answer, an error included, changes nothing for the agent.
    [[maybe_unused]] auto const reply = channel->call(request->method, request->params, *pane, left);
    return 0;
}

} // namespace contour::cli
