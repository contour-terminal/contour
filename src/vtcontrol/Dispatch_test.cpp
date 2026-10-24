// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <optional>

#include <vtcontrol/Dispatch.hpp>
#include <vtcontrol/testing/FakeControlApi.hpp>

namespace
{
using Result = std::optional<std::expected<std::string, vtcontrol::ControlError>>;

Result call(vtcontrol::ControlApi& api,
            vtcontrol::CallContext& context,
            vtcontrol::Caller caller,
            std::string_view method,
            std::string_view params)
{
    auto result = Result {};
    vtcontrol::dispatch(api, context, caller, method, params, [&](auto r) { result = std::move(r); });
    return result;
}
} // namespace

TEST_CASE("dispatch decodes, calls the typed handler and encodes", "[vtcontrol][dispatch]")
{
    auto api = vtcontrol::testing::FakeControlApi {};
    api.splitReply = vtcontrol::SplitReply { .tab = "tab:t", .pane = "pane:n" };
    auto const result = call(api,
                             api.context,
                             vtcontrol::Caller::Socket,
                             "pane.split",
                             R"({"pane":"pane:p","direction":"below"})");
    REQUIRE(result);
    REQUIRE(api.lastSplit);
    CHECK(api.lastSplit->direction == vtcontrol::SplitDirection::Below);
    CHECK(api.lastSplit->pane == "pane:p");
    REQUIRE(*result);
    CHECK(**result == R"({"tab":"tab:t","pane":"pane:n"})");
}

TEST_CASE("dispatch refusals", "[vtcontrol][dispatch]")
{
    auto api = vtcontrol::testing::FakeControlApi {};

    auto const unknown = call(api, api.context, vtcontrol::Caller::Socket, "pane.teleport", "{}");
    REQUIRE(unknown);
    REQUIRE_FALSE(*unknown);
    CHECK(unknown->error().code == vtcontrol::ControlErrorCode::MethodNotFound);
    CHECK(unknown->error().message.contains("pane.teleport"));

    auto const badParams = call(api, api.context, vtcontrol::Caller::Socket, "pane.split", R"({"pane":1})");
    REQUIRE(badParams);
    REQUIRE_FALSE(*badParams);
    CHECK(badParams->error().code == vtcontrol::ControlErrorCode::BadRequest);
    CHECK_FALSE(api.lastSplit);

    auto const emptyParams = call(api, api.context, vtcontrol::Caller::Socket, "workspace.list", "");
    REQUIRE(emptyParams);
    CHECK(*emptyParams);
    CHECK(api.lastList);

    auto const socketOnly = call(api, api.context, vtcontrol::Caller::Extension, "block.wait", "{}");
    REQUIRE(socketOnly);
    REQUIRE_FALSE(*socketOnly);
    CHECK(socketOnly->error().code == vtcontrol::ControlErrorCode::Unsupported);
    CHECK_FALSE(api.lastWait);

    auto const onSocket = call(api, api.context, vtcontrol::Caller::Socket, "block.wait", "{}");
    REQUIRE(onSocket);
    CHECK(*onSocket);
    CHECK(api.lastWait);
}

TEST_CASE("a request that runs a command needs input beside write", "[vtcontrol][dispatch][grants]")
{
    auto api = vtcontrol::testing::FakeControlApi {};
    api.context.granted = vtcontrol::ControlGrants::of(
        { vtcontrol::Permission::Read, vtcontrol::Permission::Screen, vtcontrol::Permission::Write });

    auto const shell = call(api, api.context, vtcontrol::Caller::Socket, "tab.create", R"({"cwd":"/tmp"})");
    REQUIRE(shell);
    CHECK(*shell);
    CHECK(api.lastCreateTab);

    for (auto const method: { "tab.create", "pane.split" })
    {
        INFO(method);
        api.lastCreateTab.reset();
        auto const command =
            call(api, api.context, vtcontrol::Caller::Socket, method, R"({"command":["id"]})");
        REQUIRE(command);
        REQUIRE_FALSE(*command);
        CHECK(command->error().code == vtcontrol::ControlErrorCode::Forbidden);
        CHECK(command->error().message.contains("running a command needs grant 'input'"));
        CHECK_FALSE(api.lastCreateTab);
        CHECK_FALSE(api.lastSplit);
    }

    api.context.granted = api.context.granted.with(vtcontrol::Permission::Input);
    auto const granted =
        call(api, api.context, vtcontrol::Caller::Socket, "pane.split", R"({"command":["id"]})");
    REQUIRE(granted);
    CHECK(*granted);
    CHECK(api.lastSplit);
}

TEST_CASE("a handler's failure reaches the caller as the same error", "[vtcontrol][dispatch]")
{
    auto api = vtcontrol::testing::FakeControlApi {};
    api.closeTabFailure = vtcontrol::ControlError { .code = vtcontrol::ControlErrorCode::NotFound,
                                                    .message = "no tab",
                                                    .candidates = { "tab:a", "tab:b" } };
    auto const result = call(api, api.context, vtcontrol::Caller::Socket, "tab.close", R"({"tab":"9"})");
    REQUIRE(result);
    REQUIRE_FALSE(*result);
    CHECK(result->error().code == vtcontrol::ControlErrorCode::NotFound);
    CHECK(result->error().message == "no tab");
    CHECK(result->error().candidates.size() == 2);
}

TEST_CASE("the null ControlApi serves nothing", "[vtcontrol][dispatch]")
{
    auto context = vtcontrol::testing::FakeCallContext {};
    auto& api = vtcontrol::nullControlApi();
    auto const result = call(api, context, vtcontrol::Caller::Socket, "workspace.list", "");
    REQUIRE(result);
    REQUIRE_FALSE(*result);
    CHECK(result->error().code == vtcontrol::ControlErrorCode::Unsupported);

    auto const info = call(api, context, vtcontrol::Caller::Socket, "api.list", "");
    REQUIRE(info);
    CHECK_FALSE(*info);

    // The lifecycle calls are no-ops.
    api.clientAttached("c");
    api.clientDetached("c");
    api.sinkClosed(context.sink);
    CHECK(&vtcontrol::nullControlApi() == &api);
}

TEST_CASE("api.list and api.describe are answered from the catalog by default", "[vtcontrol][dispatch]")
{
    auto api = vtcontrol::testing::FakeControlApi {};
    auto const list = call(api, api.context, vtcontrol::Caller::Socket, "api.list", "");
    REQUIRE(list);
    REQUIRE(*list);
    CHECK((*list)->contains(R"("name":"pane.split")"));

    auto const described =
        call(api, api.context, vtcontrol::Caller::Socket, "api.describe", R"({"name":"pane.split"})");
    REQUIRE(described);
    REQUIRE(*described);
    CHECK((*described)->contains(R"("inputSchema":)"));

    auto const missing =
        call(api, api.context, vtcontrol::Caller::Socket, "api.describe", R"({"name":"nope"})");
    REQUIRE(missing);
    CHECK(missing->error().code == vtcontrol::ControlErrorCode::MethodNotFound);
}
