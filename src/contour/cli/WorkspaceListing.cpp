// SPDX-License-Identifier: Apache-2.0
#include <contour/cli/WorkspaceListing.hpp>

#include <algorithm>
#include <charconv>
#include <format>
#include <ranges>
#include <utility>

#include <vtcontrol/Json.hpp>
#include <vtcontrol/ObjectRef.hpp>
#include <vtcontrol/Target.hpp>

namespace contour::cli
{

std::expected<std::vector<vtcontrol::WorkspaceInfo>, std::string> decodeWorkspaceList(
    std::string_view replyJson)
{
    return vtcontrol::decodeJson<vtcontrol::ListReply>(replyJson)
        .transform([](vtcontrol::ListReply reply) { return std::move(reply.workspaces); })
        .transform_error([](vtcontrol::ControlError const& error) { return error.message; });
}

std::string encodeCreateWorkspace(std::string const& label)
{
    return vtcontrol::encodeJson(vtcontrol::CreateWorkspaceRequest { .cwd = {}, .label = label });
}

std::expected<vtcontrol::ObjectRef, std::string> decodeCreatedWorkspace(std::string_view replyJson)
{
    return vtcontrol::decodeJson<vtcontrol::CreateWorkspaceReply>(replyJson)
        .transform_error([](vtcontrol::ControlError const& error) { return error.message; })
        .and_then([](vtcontrol::CreateWorkspaceReply const& reply)
                      -> std::expected<vtcontrol::ObjectRef, std::string> {
            if (auto const ref = vtcontrol::parseObjectRef(reply.workspace))
                return *ref;
            return std::unexpected(std::string { "the daemon named no new workspace" });
        });
}

std::expected<uint64_t, std::string> resolveWorkspaceSpec(
    std::string_view spec, std::span<vtcontrol::WorkspaceInfo const> workspaces)
{
    auto const refOf = [](vtcontrol::WorkspaceInfo const& info) {
        return vtcontrol::parseObjectRef(info.id).value_or(vtcontrol::ObjectRef {});
    };
    auto const candidates = std::ranges::to<std::vector>(
        workspaces | std::views::transform([&](vtcontrol::WorkspaceInfo const& info) {
            return vtcontrol::TargetCandidate { .ref = refOf(info),
                                                .label = info.label,
                                                .position = info.position };
        }));
    auto const resolved = vtcontrol::resolveTarget(vtcontrol::ObjectKind::Workspace, spec, candidates);
    if (!resolved)
        return std::unexpected(resolved.error().message);
    auto const found = std::ranges::find_if(
        workspaces, [&](vtcontrol::WorkspaceInfo const& info) { return refOf(info) == *resolved; });
    auto id = uint64_t {};
    if (found == workspaces.end())
        return std::unexpected(std::format("no workspace '{}'", spec));
    auto const* const end = found->native.data() + found->native.size();
    if (auto const [last, error] = std::from_chars(found->native.data(), end, id);
        error != std::errc {} || last != end || id == 0)
        return std::unexpected(std::format("the daemon is too old to attach to workspace '{}' alone", spec));
    return id;
}

} // namespace contour::cli
