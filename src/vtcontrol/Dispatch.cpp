// SPDX-License-Identifier: Apache-2.0
#include <vtcontrol/Dispatch.hpp>

#include <format>
#include <tuple>

#include <vtcontrol/Json.hpp>

namespace vtcontrol
{

namespace
{
    [[nodiscard]] std::unexpected<ControlError> failure(ControlErrorCode code, std::string message)
    {
        return std::unexpected(
            ControlError { .code = code, .message = std::move(message), .candidates = {} });
    }

    /// Serves the call when `row` is the method called.
    /// @return Whether `row` was the method, and so whether `done` has been consumed.
    template <typename Row>
    [[nodiscard]] bool serve(Row const& row,
                             ControlApi& api,
                             CallContext& context,
                             Caller caller,
                             std::string_view method,
                             std::string_view params,
                             RawCompletion& done)
    {
        if (row.name != method)
            return false;

        if (row.exposure == Exposure::SocketOnly && caller == Caller::Extension)
        {
            done(failure(ControlErrorCode::Unsupported,
                         std::format("method '{}' is not available to extensions", method)));
            return true;
        }

        if (!context.grants().contains(row.permission))
        {
            done(failure(ControlErrorCode::Forbidden,
                         std::format("method '{}' needs grant '{}'", method, nameOf(row.permission))));
            return true;
        }

        auto request = decodeJson<typename Row::Request>(params);
        if (!request)
        {
            done(std::unexpected(std::move(request.error())));
            return true;
        }

        if (auto const extra = row.requestGrant(*request);
            extra && !context.grants().contains(extra->permission))
        {
            done(failure(
                ControlErrorCode::Forbidden,
                std::format(
                    "method '{}': {} needs grant '{}'", method, extra->reason, nameOf(extra->permission))));
            return true;
        }

        using Reply = Row::Reply;
        api.handle(
            context,
            *request,
            Completion<Reply> { [done = std::move(done)](std::expected<Reply, ControlError> result) mutable {
                if (result)
                    done(encodeJson(*result));
                else
                    done(std::unexpected(std::move(result.error())));
            } });
        return true;
    }
} // namespace

void dispatch(ControlApi& api,
              CallContext& context,
              Caller caller,
              std::string_view method,
              std::string_view params,
              RawCompletion done)
{
    auto const served = std::apply(
        [&](auto const&... row) { return (serve(row, api, context, caller, method, params, done) || ...); },
        Methods);
    if (!served)
        done(failure(ControlErrorCode::MethodNotFound, std::format("no method '{}'", method)));
}

} // namespace vtcontrol
