// SPDX-License-Identifier: Apache-2.0
#include <vthost/control/ControlService.hpp>

#include <vtbackend/screen/Terminal.hpp>

#include <core/Overloaded.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <limits>
#include <memory>
#include <mutex>
#include <ranges>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

#include <vtcontrol/Events.hpp>
#include <vtcontrol/Json.hpp>
#include <vtcontrol/ObjectRef.hpp>
#include <vthost/NodeMove.hpp>
#include <vthost/ViewRouting.hpp>
#include <vthost/WorkspaceVisibility.hpp>
#include <vthost/control/PaneText.hpp>
#include <vthost/control/ProgramStatusConvert.hpp>
#include <vthost/control/StatusText.hpp>
#include <vthost/proto/Negotiation.hpp>
#include <vtworkspace/Pane.hpp>
#include <vtworkspace/SessionModel.hpp>
#include <vtworkspace/Tab.hpp>

namespace vthost::control
{

using vtcontrol::ControlError;
using vtcontrol::ControlErrorCode;
using vtcontrol::ObjectKind;
using vtcontrol::ObjectRef;
using vtcontrol::TargetCandidate;
using vtworkspace::SessionId;
using vtworkspace::TabId;
using vtworkspace::WorkspaceId;

namespace
{
    /// The operating system the daemon runs on, as `daemon.info` names it.
    constexpr auto PlatformName = std::string_view {
#ifdef _WIN32
        "workspaces"
#elifdef __APPLE__
        "macos"
#elifdef __linux__
        "linux"
#elifdef __FreeBSD__
        "freebsd"
#else
        "unknown"
#endif
    };

    /// What a pane's program-status records need, in a listing as in their event: they name programs and say
    /// what those are doing, as a command line does, so a listing hands them out by the same rule.
    constexpr auto ProgramStatusPermission =
        std::get<vtcontrol::EventRow<vtcontrol::ProgramStatusChangedEvent>>(vtcontrol::EventCatalog)
            .permission;

    [[nodiscard]] ControlError notFound(std::string message)
    {
        return ControlError { .code = ControlErrorCode::NotFound,
                              .message = std::move(message),
                              .candidates = {} };
    }

    /// @return @p done as a waiter's completion: the block it ended with becomes the reply's `block`.
    /// @tparam Reply A reply whose one member is the optional block, `WaitReply` or `SendReply`.
    template <typename Reply>
    [[nodiscard]] vtcontrol::Completion<vtcontrol::BlockInfo> replyingWith(vtcontrol::Completion<Reply> done)
    {
        return [done = std::move(done)](std::expected<vtcontrol::BlockInfo, ControlError> block) mutable {
            done(std::move(block).transform(
                [](vtcontrol::BlockInfo&& info) { return Reply { .block = std::move(info) }; }));
        };
    }

    [[nodiscard]] ControlError failed(std::string message)
    {
        return ControlError { .code = ControlErrorCode::Failed,
                              .message = std::move(message),
                              .candidates = {} };
    }

    /// The narrowest and widest share a split may give the new pane, in percent.
    constexpr auto MinimumSplitPercent = uint8_t { 5 };
    constexpr auto MaximumSplitPercent = uint8_t { 95 };

    /// @return The text of a workspace, tab or pane reference.
    [[nodiscard]] std::string idOf(ObjectKind kind, vtcontrol::Uuid uuid)
    {
        return vtcontrol::toString(ObjectRef { .kind = kind, .uuid = uuid });
    }

    /// @param command The program and its arguments; empty for the configured shell.
    /// @param cwd The working directory; empty for the default.
    /// @return What a spawn the API asks for overrides.
    [[nodiscard]] SessionSpawnRequest spawnRequestOf(std::vector<std::string> const& command,
                                                     std::string const& cwd)
    {
        auto request = SessionSpawnRequest {};
        if (command.empty() && cwd.empty())
            return request;
        auto exec = vtpty::Process::ExecInfo {};
        if (!command.empty())
        {
            exec.program = command.front();
            exec.arguments.assign(std::next(command.begin()), command.end());
        }
        exec.workingDirectory = cwd;
        request.commandOverride = std::move(exec);
        return request;
    }

    /// @return A count as an API `u32`, saturated.
    [[nodiscard]] uint32_t apiCount(std::size_t count) noexcept
    {
        return static_cast<uint32_t>(std::min<std::size_t>(count, std::numeric_limits<uint32_t>::max()));
    }

    /// Calls @p visit with every leaf of @p tab, in tree order.
    template <typename Visit>
    void forEachLeaf(vtworkspace::Tab const& tab, Visit const& visit)
    {
        tab.rootPane()->walkTree([&](vtworkspace::Pane& pane) {
            if (pane.isLeaf())
                visit(pane);
        });
    }

    /// @return The record a block id names in @p store, or nothing when the id is of another epoch or gone.
    [[nodiscard]] vtbackend::CommandBlockRecord const* findBlock(vtbackend::CommandBlockStore const& store,
                                                                 uint64_t serial) noexcept
    {
        if ((serial >> 32) != store.idEpoch())
            return nullptr;
        return store.find(vtbackend::CommandBlockId { static_cast<uint32_t>(serial & 0xFFFF'FFFFU) });
    }

    /// @return Which part of a block the terminal reads for @p part.
    [[nodiscard]] std::string blockPartText(vtbackend::Terminal const& terminal,
                                            vtbackend::CommandBlockId block,
                                            vtbackend::CommandBlockPart part)
    {
        // A part the block has none of, or whose rows have left the grid, reads as empty: the record still
        // says what the block was.
        return terminal
            .commandBlockText(
                vtbackend::CommandBlockTarget::Last, part, vtbackend::CaptureRendition::PlainText, block)
            .value_or(std::string {});
    }
} // namespace

ControlService::ControlService(SessionHost& host, ControlServiceConfig config):
    _host(host),
    _config(std::move(config)),
    _startedAt(_config.clock.now()),
    _hub(_config.maxSubscriptionsPerConnection),
    _waiters(_config.maxWaitsPerConnection),
    _streamSubscription(makeScopedStreamSubscription(host, *this)),
    _modelSubscription(makeScopedModelSubscription(host, *this)),
    _labelSubscription(host.objects().onLabelChanged(
        [this](ObjectRef const& ref, std::string_view label) { labelChanged(ref, label); })),
    _extensionControl(makeScopedExtensionControl(host, *this))
{
}

ControlService::~ControlService()
{
    // Completions the host still holds must find nothing to answer.
    _switchCalls->waiting.clear();
}

// {{{ Target resolution

std::vector<TargetCandidate> ControlService::paneCandidates() const
{
    auto candidates = std::vector<TargetCandidate> {};
    _host.model().forEachTab([&](vtworkspace::Workspace&, vtworkspace::Tab& tab) {
        forEachLeaf(tab, [&](vtworkspace::Pane const& leaf) {
            if (auto const uuid = _host.objects().uuidOf(leaf.session()))
            {
                auto const ref = ObjectRef { .kind = ObjectKind::Pane, .uuid = *uuid };
                candidates.push_back(TargetCandidate { .ref = ref,
                                                       .label = std::string { _host.objects().labelOf(ref) },
                                                       .position = std::nullopt });
            }
        });
    });
    return candidates;
}

std::vector<TargetCandidate> ControlService::tabCandidates(WorkspaceId positioned) const
{
    auto candidates = std::vector<TargetCandidate> {};
    _host.model().forEachTab([&](vtworkspace::Workspace& workspace, vtworkspace::Tab& tab) {
        auto const uuid = _host.objects().uuidOf(tab.id());
        if (!uuid)
            return;
        auto const ref = ObjectRef { .kind = ObjectKind::Tab, .uuid = *uuid };
        auto position = std::optional<uint32_t> {};
        if (workspace.id() == positioned)
            position = static_cast<uint32_t>(workspace.indexOf(tab.id()) + 1);
        candidates.push_back(TargetCandidate {
            .ref = ref, .label = std::string { _host.objects().labelOf(ref) }, .position = position });
    });
    return candidates;
}

std::vector<TargetCandidate> ControlService::workspaceCandidates() const
{
    auto candidates = std::vector<TargetCandidate> {};
    auto const& model = _host.model();
    for (auto const index: std::views::iota(0, model.workspaceCount()))
    {
        auto const* workspace = model.workspaceAt(index);
        auto const uuid = _host.objects().uuidOf(workspace->id());
        if (!uuid)
            continue;
        auto const ref = ObjectRef { .kind = ObjectKind::Workspace, .uuid = *uuid };
        candidates.push_back(TargetCandidate { .ref = ref,
                                               .label = std::string { _host.objects().labelOf(ref) },
                                               .position = static_cast<uint32_t>(index + 1) });
    }
    return candidates;
}

std::expected<ControlService::PaneTarget, ControlError> ControlService::resolvePane(
    vtcontrol::CallContext const& context, std::string_view spec) const
{
    // An empty target is the caller's own pane. A caller that names a pane that is gone gets NotFound for it,
    // never another pane: a script outliving its pane must not type into whatever is active now.
    auto const named = spec.empty() ? context.caller() : spec;
    if (named.empty())
    {
        auto const* workspace = _host.model().workspace(_host.workspaceId());
        auto const* tab = workspace != nullptr ? workspace->activeTab() : nullptr;
        auto const* leaf = tab != nullptr ? tab->activePane() : nullptr;
        if (leaf == nullptr)
            return std::unexpected(notFound("there is no active pane"));
        auto const uuid = _host.objects().uuidOf(leaf->session());
        if (!uuid)
            return std::unexpected(notFound("the active pane has no id"));
        return livePane(leaf->session(), *uuid, "the active pane");
    }

    auto const candidates = paneCandidates();
    return vtcontrol::resolveTarget(ObjectKind::Pane, named, candidates)
        .and_then([&](ObjectRef const& ref) -> std::expected<PaneTarget, ControlError> {
            auto const handle = _host.objects().find(ref);
            auto const* session = handle ? std::get_if<SessionId>(&*handle) : nullptr;
            if (session == nullptr)
                return std::unexpected(notFound(std::format("pane '{}' is gone", named)));
            return livePane(*session, ref.uuid, std::format("pane '{}'", named));
        });
}

std::expected<ControlService::PaneTarget, ControlError> ControlService::livePane(SessionId session,
                                                                                 vtcontrol::Uuid uuid,
                                                                                 std::string_view what) const
{
    // Every pane a call goes on to read has a terminal: the one check that lets them dereference it.
    if (_host.terminal(session) == nullptr)
        return std::unexpected(notFound(std::format("{} is gone", what)));
    return PaneTarget { .session = session, .uuid = uuid };
}

std::expected<TabId, ControlError> ControlService::resolveTab(vtcontrol::CallContext const& context,
                                                              std::string_view spec) const
{
    if (spec.empty())
        return resolvePane(context, {})
            .and_then([&](PaneTarget const& pane) -> std::expected<TabId, ControlError> {
                auto const [workspace, tab, leaf] = _host.model().findSessionLeaf(pane.session);
                if (tab == nullptr)
                    return std::unexpected(notFound("the pane is in no tab"));
                return tab->id();
            });

    auto const positioned = resolveWorkspace(context, {}).value_or(_host.workspaceId());
    auto const candidates = tabCandidates(positioned);
    return vtcontrol::resolveTarget(ObjectKind::Tab, spec, candidates)
        .and_then([&](ObjectRef const& ref) -> std::expected<TabId, ControlError> {
            auto const handle = _host.objects().find(ref);
            auto const* tab = handle ? std::get_if<TabId>(&*handle) : nullptr;
            if (tab == nullptr || _host.model().findTab(*tab) == nullptr)
                return std::unexpected(notFound(std::format("tab '{}' is gone", spec)));
            return *tab;
        });
}

std::expected<WorkspaceId, ControlError> ControlService::resolveWorkspace(
    vtcontrol::CallContext const& context, std::string_view spec) const
{
    if (spec.empty())
    {
        auto const pane = resolvePane(context, {});
        // A daemon whose workspace holds no pane yet still has a workspace to put a tab in; a caller whose
        // pane is gone has none.
        if (!pane)
            return context.caller().empty() ? std::expected<WorkspaceId, ControlError> { _host.workspaceId() }
                                            : std::unexpected(pane.error());
        auto const [workspace, tab, leaf] = _host.model().findSessionLeaf(pane->session);
        if (workspace == nullptr)
            return std::unexpected(notFound("the pane is in no workspace"));
        return workspace->id();
    }

    auto const candidates = workspaceCandidates();
    return vtcontrol::resolveTarget(ObjectKind::Workspace, spec, candidates)
        .and_then([&](ObjectRef const& ref) -> std::expected<WorkspaceId, ControlError> {
            auto const handle = _host.objects().find(ref);
            auto const* workspace = handle ? std::get_if<WorkspaceId>(&*handle) : nullptr;
            if (workspace == nullptr || _host.model().workspace(*workspace) == nullptr)
                return std::unexpected(notFound(std::format("workspace '{}' is gone", spec)));
            return *workspace;
        });
}

// }}}
// {{{ Records

vtcontrol::PaneInfo ControlService::paneInfoOf(SessionId session,
                                               vtworkspace::Tab const& tab,
                                               vtcontrol::ControlGrants grants) const
{
    auto const uuid = _host.objects().uuidOf(session).value_or(vtcontrol::Uuid {});
    auto const ref = ObjectRef { .kind = ObjectKind::Pane, .uuid = uuid };
    auto info = vtcontrol::PaneInfo {};
    info.id = vtcontrol::toString(ref);
    info.label = std::string { _host.objects().labelOf(ref) };
    info.isActive = tab.activePane() != nullptr && tab.activePane()->session() == session;
    if (auto const* terminal = _host.terminal(session))
    {
        auto const guard = std::lock_guard { std::as_const(*terminal) };
        info.title = vtcontrol::toValidUtf8(terminal->windowTitle());
        if (auto const cwd = terminal->resolveWorkingDirectory(vtbackend::CwdPurpose::Display))
            info.cwd = vtcontrol::toValidUtf8(cwd->path);
        auto const size = terminal->pageSize();
        info.size = vtcontrol::Size { .columns = unbox<uint32_t>(size.columns),
                                      .lines = unbox<uint32_t>(size.lines) };
        if (grants.contains(ProgramStatusPermission))
            info.programStatus = toApi(terminal->programStatus());
    }
    return info;
}

vtcontrol::WorkspaceInfo ControlService::workspaceInfoOf(vtworkspace::Workspace const& workspace,
                                                         uint32_t position,
                                                         vtcontrol::ControlGrants grants) const
{
    auto const workspaceRef =
        ObjectRef { .kind = ObjectKind::Workspace,
                    .uuid = _host.objects().uuidOf(workspace.id()).value_or(vtcontrol::Uuid {}) };
    auto info = vtcontrol::WorkspaceInfo {};
    info.id = vtcontrol::toString(workspaceRef);
    info.label = std::string { _host.objects().labelOf(workspaceRef) };
    info.position = position;
    info.native = std::to_string(workspace.id().value);
    info.visibility = std::string { nameOf(_host.visibility(workspace.id().value)) };
    info.clients = static_cast<uint32_t>(_host.clientsShowing(workspace.id().value).size());

    auto const titleOf = [&](SessionId session) -> std::string {
        auto const* terminal = _host.terminal(session);
        if (terminal == nullptr)
            return {};
        auto const guard = std::lock_guard { std::as_const(*terminal) };
        return terminal->windowTitle();
    };
    for (auto const index: std::views::iota(0, workspace.tabCount()))
    {
        auto const* tab = workspace.tabAt(index);
        auto const tabRef =
            ObjectRef { .kind = ObjectKind::Tab,
                        .uuid = _host.objects().uuidOf(tab->id()).value_or(vtcontrol::Uuid {}) };
        auto tabInfo = vtcontrol::TabInfo {};
        tabInfo.id = vtcontrol::toString(tabRef);
        tabInfo.label = std::string { _host.objects().labelOf(tabRef) };
        tabInfo.title = vtcontrol::toValidUtf8(tab->title(titleOf));
        tabInfo.position = static_cast<uint32_t>(index + 1);
        tabInfo.isActive = workspace.activeTab() == tab;
        forEachLeaf(*tab, [&](vtworkspace::Pane const& leaf) {
            tabInfo.panes.push_back(paneInfoOf(leaf.session(), *tab, grants));
        });
        info.tabs.push_back(std::move(tabInfo));
    }
    return info;
}

// }}}
// {{{ Read verbs

void ControlService::handle(vtcontrol::CallContext& context,
                            vtcontrol::DaemonInfoRequest const& /*request*/,
                            vtcontrol::Completion<vtcontrol::DaemonInfoReply> done)
{
    auto const uptime = std::chrono::duration_cast<std::chrono::seconds>(_config.clock.now() - _startedAt);
    auto const held = context.grants();
    auto grants = std::vector<std::string> {};
    for (auto const& row: vtcontrol::EnumTable<vtcontrol::Permission>::Rows)
        if (held.contains(row.value))
            grants.emplace_back(row.name);
    done(vtcontrol::DaemonInfoReply {
        .version = _config.version,
        .protocolVersion = proto::SupportedVersions.max,
        .platform = std::string { PlatformName },
        .hostname = vtcontrol::toValidUtf8(_host.identity().hostname),
        .uptimeSeconds = apiCount(static_cast<std::size_t>(std::max<int64_t>(uptime.count(), 0))),
        .grants = std::move(grants),
    });
}

namespace
{
    /// @param error Why an upgrade did not happen.
    /// @return The control error that reports it: `failed`, its message leading with the refusal's stable
    ///         name (`sandboxed: ...`) and then saying why in the words every surface uses.
    [[nodiscard]] vtcontrol::ControlError upgradeErrorOf(handover::UpgradeError const& error)
    {
        return std::visit(
            [](auto const& failure) {
                using Failure = std::decay_t<decltype(failure)>;
                if constexpr (std::is_same_v<Failure, handover::UpgradeRefusal>)
                    return vtcontrol::ControlError { .code = vtcontrol::ControlErrorCode::Failed,
                                                     .message = std::format("{}: {}",
                                                                            handover::nameOf(failure),
                                                                            handover::describe(failure)),
                                                     .candidates = {} };
                else
                    return vtcontrol::ControlError { .code = vtcontrol::ControlErrorCode::Failed,
                                                     .message = std::format("{}: {}: {}",
                                                                            handover::ExecFailureName,
                                                                            handover::ExecFailureText,
                                                                            failure.cause.message()),
                                                     .candidates = {} };
            },
            error);
    }
} // namespace

void ControlService::handle(vtcontrol::CallContext& context,
                            vtcontrol::DaemonUpgradeRequest const& request,
                            vtcontrol::Completion<vtcontrol::DaemonUpgradeReply> done)
{
    using Answer = std::shared_ptr<vtcontrol::Completion<vtcontrol::DaemonUpgradeReply>>;
    // Only a peer on this machine may replace the daemon's binary: a network peer's token is not that.
    if (context.locality() != vtcontrol::CallerLocality::Local)
    {
        done(std::unexpected(upgradeErrorOf(handover::UpgradeError { handover::UpgradeRefusal::LocalOnly })));
        return;
    }
    if (!_config.upgrade)
    {
        done(std::unexpected(
            upgradeErrorOf(handover::UpgradeError { handover::UpgradeRefusal::NotSupported })));
        return;
    }
    // Answered once: "started" comes first, ahead of the announcement and of the closing of this very
    // connection, and a failure after it (the exec) reaches nobody.
    auto const answer =
        std::make_shared<vtcontrol::Completion<vtcontrol::DaemonUpgradeReply>>(std::move(done));
    auto const answerOnce = [](Answer const& pending, auto reply) {
        if (auto const complete = std::exchange(*pending, nullptr))
            complete(std::move(reply));
    };
    _config.upgrade(
        handover::UpgradeTarget { .imageId = request.image },
        [answer, answerOnce](std::string_view toRelease) {
            answerOnce(answer,
                       std::expected<vtcontrol::DaemonUpgradeReply, vtcontrol::ControlError> {
                           vtcontrol::DaemonUpgradeReply { .started = true,
                                                           .toRelease = std::string { toRelease } } });
        },
        [answer, answerOnce](handover::UpgradeError const& error) {
            answerOnce(answer,
                       std::expected<vtcontrol::DaemonUpgradeReply, vtcontrol::ControlError> {
                           std::unexpected(upgradeErrorOf(error)) });
        });
}

void ControlService::handle(vtcontrol::CallContext& context,
                            vtcontrol::ListRequest const& request,
                            vtcontrol::Completion<vtcontrol::ListReply> done)
{
    done(list(context, request));
}

void ControlService::handle(vtcontrol::CallContext& context,
                            vtcontrol::CaptureRequest const& request,
                            vtcontrol::Completion<vtcontrol::CaptureReply> done)
{
    if (request.scrollback < -1)
    {
        done(std::unexpected(ControlError {
            .code = ControlErrorCode::BadRequest,
            .message = std::format("scrollback {} is neither -1 (all) nor a line count", request.scrollback),
            .candidates = {} }));
        return;
    }
    done(resolvePane(context, request.pane).transform([&](PaneTarget const& pane) {
        return capture(pane, request.scrollback);
    }));
}

void ControlService::handle(vtcontrol::CallContext& context,
                            vtcontrol::BlockListRequest const& request,
                            vtcontrol::Completion<vtcontrol::BlockListReply> done)
{
    done(resolvePane(context, request.pane).transform([&](PaneTarget const& pane) {
        return listBlocks(pane, request.limit);
    }));
}

void ControlService::handle(vtcontrol::CallContext& context,
                            vtcontrol::BlockReadRequest const& request,
                            vtcontrol::Completion<vtcontrol::BlockReadReply> done)
{
    done(resolveBlock(context, request.block, request.pane).and_then([&](BlockTarget const& block) {
        return readBlock(block, request.part);
    }));
}

void ControlService::handle(vtcontrol::CallContext& context,
                            vtcontrol::SetLabelRequest const& request,
                            vtcontrol::Completion<vtcontrol::EmptyReply> done)
{
    done(resolveObject(context, request.object).transform([&](ObjectRef const& ref) {
        _host.objects().setLabel(ref, request.label);
        return vtcontrol::EmptyReply {};
    }));
}

std::expected<vtcontrol::ListReply, ControlError> ControlService::list(
    vtcontrol::CallContext const& context, vtcontrol::ListRequest const& request) const
{
    auto only = std::optional<WorkspaceId> {};
    if (!request.workspace.empty())
    {
        auto const workspace = resolveWorkspace(context, request.workspace);
        if (!workspace)
            return std::unexpected(workspace.error());
        only = *workspace;
    }
    auto reply = vtcontrol::ListReply {};
    auto const& model = _host.model();
    for (auto const index: std::views::iota(0, model.workspaceCount()))
    {
        auto const* workspace = model.workspaceAt(index);
        if (!only || workspace->id() == *only)
            reply.workspaces.push_back(
                workspaceInfoOf(*workspace, static_cast<uint32_t>(index + 1), context.grants()));
    }
    return reply;
}

vtcontrol::CaptureReply ControlService::capture(PaneTarget const& pane, int32_t scrollback) const
{
    auto const* terminal = _host.terminal(pane.session);
    auto reply = vtcontrol::CaptureReply {};
    reply.pane = idOf(ObjectKind::Pane, pane.uuid);
    auto raw = std::string {};
    {
        auto const guard = std::lock_guard { std::as_const(*terminal) };
        raw = captureText(*terminal, scrollback);
        auto const& cursor = terminal->currentScreen().cursor().position;
        reply.cursor = vtcontrol::Position { .line = unbox<int32_t>(cursor.line),
                                             .column = unbox<uint32_t>(cursor.column) };
        auto const size = terminal->pageSize();
        reply.size = vtcontrol::Size { .columns = unbox<uint32_t>(size.columns),
                                       .lines = unbox<uint32_t>(size.lines) };
    }
    auto bounded = keepTail(vtcontrol::toValidUtf8(raw), _config.maxReplyBytes);
    reply.text = std::move(bounded.text);
    reply.truncated = bounded.truncated;
    return reply;
}

vtcontrol::BlockListReply ControlService::listBlocks(PaneTarget const& pane, uint32_t limit) const
{
    auto const* terminal = _host.terminal(pane.session);
    auto reply = vtcontrol::BlockListReply {};
    reply.pane = idOf(ObjectKind::Pane, pane.uuid);
    auto const guard = std::lock_guard { std::as_const(*terminal) };
    auto const& store = terminal->commandBlocks();
    for (auto const& record: store.records() | std::views::reverse | std::views::take(limit))
        reply.blocks.push_back(blockInfoOf(pane.uuid, record, store.idEpoch()));
    return reply;
}

std::expected<ControlService::BlockTarget, ControlError> ControlService::resolveBlock(
    vtcontrol::CallContext const& context, std::string_view block, std::string_view pane) const
{
    // No block names the pane's last finished one.
    if (block.empty())
        return resolvePane(context, pane).transform([](PaneTarget const& target) {
            return BlockTarget { .pane = target, .serial = std::nullopt };
        });

    // A block names its pane: a pane that is gone takes its blocks with it.
    auto const ref = vtcontrol::parseBlockRef(block);
    if (!ref)
        return std::unexpected(notFound(std::format("'{}' is not a block id", block)));
    auto const target = resolvePane(context, idOf(ObjectKind::Pane, ref->pane));
    if (!target)
        return std::unexpected(notFound(std::format("block '{}': {}", block, target.error().message)));
    return BlockTarget { .pane = *target, .serial = ref->serial };
}

std::expected<vtcontrol::BlockReadReply, ControlError> ControlService::readBlock(
    BlockTarget const& block, vtcontrol::BlockPart part) const
{
    auto const* terminal = _host.terminal(block.pane.session);
    auto reply = vtcontrol::BlockReadReply {};
    auto prompt = std::string {};
    auto output = std::string {};
    {
        auto const guard = std::lock_guard { std::as_const(*terminal) };
        auto const& store = terminal->commandBlocks();
        auto const* record = block.serial ? findBlock(store, *block.serial) : store.lastFinished();
        if (record == nullptr)
            return std::unexpected(
                notFound(block.serial ? std::format("no block '{}'",
                                                    vtcontrol::toString(vtcontrol::BlockRef {
                                                        .pane = block.pane.uuid, .serial = *block.serial }))
                                      : std::format("pane '{}' has no finished block",
                                                    idOf(ObjectKind::Pane, block.pane.uuid))));
        reply.block = blockInfoOf(block.pane.uuid, *record, store.idEpoch());
        if (part != vtcontrol::BlockPart::Output)
            prompt = blockPartText(*terminal, record->id, vtbackend::CommandBlockPart::Prompt);
        if (part != vtcontrol::BlockPart::Prompt)
            output = blockPartText(*terminal, record->id, vtbackend::CommandBlockPart::Output);
    }
    auto [boundedPrompt, boundedOutput] =
        boundBlockText(vtcontrol::toValidUtf8(prompt), vtcontrol::toValidUtf8(output), _config.maxReplyBytes);
    reply.prompt = std::move(boundedPrompt.text);
    reply.output = std::move(boundedOutput.text);
    reply.truncated = boundedPrompt.truncated == vtcontrol::Truncation::Head
                              || boundedOutput.truncated == vtcontrol::Truncation::Head
                          ? vtcontrol::Truncation::Head
                          : vtcontrol::Truncation::None;
    return reply;
}

std::expected<ObjectRef, ControlError> ControlService::resolveObject(vtcontrol::CallContext const& context,
                                                                     std::string_view object) const
{
    // `<kind>:<spec>`: a full id is resolved as one, anything else after the kind as a spec of that kind.
    auto const colon = object.find(':');
    auto const kind = colon == std::string_view::npos
                          ? std::nullopt
                          : vtcontrol::enumFromName<ObjectKind>(object.substr(0, colon));
    if (!kind)
        return std::unexpected(ControlError {
            .code = ControlErrorCode::BadRequest,
            .message = std::format("'{}' names no workspace, tab or pane; write <kind>:<target>", object),
            .candidates = {} });
    auto const spec = vtcontrol::parseObjectRef(object) ? object : object.substr(colon + 1);
    switch (*kind)
    {
        case ObjectKind::Pane:
            return resolvePane(context, spec).transform([](PaneTarget const& pane) {
                return ObjectRef { .kind = ObjectKind::Pane, .uuid = pane.uuid };
            });
        case ObjectKind::Tab:
            return resolveTab(context, spec).transform([&](TabId tab) {
                return ObjectRef { .kind = ObjectKind::Tab, .uuid = *_host.objects().uuidOf(tab) };
            });
        case ObjectKind::Workspace:
            return resolveWorkspace(context, spec).transform([&](WorkspaceId workspace) {
                return ObjectRef { .kind = ObjectKind::Workspace,
                                   .uuid = *_host.objects().uuidOf(workspace) };
            });
    }
    return std::unexpected(notFound(std::format("'{}' names no object", object)));
}

// }}}
// {{{ Write verbs

void ControlService::handle(vtcontrol::CallContext& context,
                            vtcontrol::CreateTabRequest const& request,
                            vtcontrol::Completion<vtcontrol::CreateTabReply> done)
{
    done(createTab(context, request));
}

void ControlService::handle(vtcontrol::CallContext& context,
                            vtcontrol::SplitRequest const& request,
                            vtcontrol::Completion<vtcontrol::SplitReply> done)
{
    done(split(context, request));
}

void ControlService::handle(vtcontrol::CallContext& context,
                            vtcontrol::SendRequest const& request,
                            vtcontrol::Completion<vtcontrol::SendReply> done)
{
    auto const pane = resolvePane(context, request.pane);
    if (pane && request.wait == vtcontrol::SendWait::CommandFinished)
    {
        sendAndWait(context, *pane, request.text, std::move(done));
        return;
    }
    done(pane.transform([&](PaneTarget const& target) {
        if (!request.text.empty())
            _host.writeInput(target.session, request.text);
        return vtcontrol::SendReply {};
    }));
}

namespace
{
    /// The longest integration id.
    constexpr auto MaxIntegrationIdLength = std::size_t { 32 };

    /// @return Whether @p id is an integration id: [a-z0-9-]{1,32}.
    [[nodiscard]] bool isIntegrationId(std::string_view id) noexcept
    {
        return !id.empty() && id.size() <= MaxIntegrationIdLength && std::ranges::all_of(id, [](char ch) {
            return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-';
        });
    }

    [[nodiscard]] ControlError badStatusRequest(std::string message)
    {
        return ControlError { .code = ControlErrorCode::BadRequest,
                              .message = std::move(message),
                              .candidates = {} };
    }

    /// @return Nothing when @p id is an integration id; else BadRequest saying what one is.
    [[nodiscard]] std::expected<void, ControlError> checkIntegrationId(std::string_view id)
    {
        if (isIntegrationId(id))
            return {};
        return std::unexpected(badStatusRequest("'integration' must match [a-z0-9-]{1,32}"));
    }

    /// @return The record @p request asks for, or BadRequest naming what is wrong with it.
    [[nodiscard]] std::expected<vtbackend::ProgramStatusRecord, ControlError> integrationRecordOf(
        vtcontrol::ReportStatusRequest const& request)
    {
        if (auto checked = checkIntegrationId(request.integration); !checked)
            return std::unexpected(std::move(checked.error()));
        auto const state = vtcontrol::enumFromName<vtcontrol::ProgramState>(request.state);
        if (!state)
            return std::unexpected(badStatusRequest(std::format("unknown state '{}'", request.state)));
        auto const kind = request.kind.empty()
                              ? std::optional { vtcontrol::BlockedKind::None }
                              : vtcontrol::enumFromName<vtcontrol::BlockedKind>(request.kind);
        if (!kind)
            return std::unexpected(badStatusRequest(std::format("unknown kind '{}'", request.kind)));
        if (*kind != vtcontrol::BlockedKind::None && *state != vtcontrol::ProgramState::Blocked)
            return std::unexpected(badStatusRequest("'kind' is for the blocked state only"));
        if (request.progress && *request.progress > 100)
            return std::unexpected(badStatusRequest("'progress' must be 0..100"));
        auto record = vtbackend::ProgramStatusRecord {
            .id = request.integration,
            .state = fromApi(ProgramStateBridge, *state),
            .kind = fromApi(BlockedKindBridge, *kind),
            .progress = std::nullopt,
            // Its own app, the integration: an empty one would inherit the root record's (OSC 7501's) app,
            // so a shell's "vim" would name the agent.
            .app = request.integration,
            .title = sanitizeStatusText(request.title),
            .message = sanitizeStatusText(request.message),
            .origin = vtbackend::ProgramStatusOrigin::Integration,
        };
        if (vtbackend::takesProgress(record.state))
            record.progress = request.progress;
        return record;
    }
} // namespace

void ControlService::handle(vtcontrol::CallContext& context,
                            vtcontrol::ReportStatusRequest const& request,
                            vtcontrol::Completion<vtcontrol::EmptyReply> done)
{
    done(resolvePane(context, request.pane).and_then([&](PaneTarget const& pane) {
        return integrationRecordOf(request).and_then(
            [&](vtbackend::ProgramStatusRecord record) -> std::expected<vtcontrol::EmptyReply, ControlError> {
                auto* const terminal = _host.terminal(pane.session);
                if (terminal == nullptr)
                    return std::unexpected(notFound("the pane has no terminal"));
                std::ignore = terminal->applyIntegrationStatus(std::move(record));
                return vtcontrol::EmptyReply {};
            });
    }));
}

void ControlService::handle(vtcontrol::CallContext& context,
                            vtcontrol::ClearStatusRequest const& request,
                            vtcontrol::Completion<vtcontrol::EmptyReply> done)
{
    done(checkIntegrationId(request.integration)
             .and_then([&] { return resolvePane(context, request.pane); })
             .and_then([&](PaneTarget const& pane) -> std::expected<vtcontrol::EmptyReply, ControlError> {
                 auto* const terminal = _host.terminal(pane.session);
                 if (terminal == nullptr)
                     return std::unexpected(notFound("the pane has no terminal"));
                 std::ignore = terminal->clearIntegrationStatus(request.integration);
                 return vtcontrol::EmptyReply {};
             }));
}

void ControlService::handle(vtcontrol::CallContext& context,
                            vtcontrol::ClosePaneRequest const& request,
                            vtcontrol::Completion<vtcontrol::EmptyReply> done)
{
    done(resolvePane(context, request.pane).transform([&](PaneTarget const& pane) {
        closePane(pane.session);
        return vtcontrol::EmptyReply {};
    }));
}

void ControlService::handle(vtcontrol::CallContext& context,
                            vtcontrol::CloseTabRequest const& request,
                            vtcontrol::Completion<vtcontrol::EmptyReply> done)
{
    done(resolveTab(context, request.tab).transform([&](TabId tabId) {
        // Collected first: closing a pane changes the tree being walked, and the last one closes the tab.
        auto sessions = std::vector<SessionId> {};
        if (auto const* tab = _host.model().findTab(tabId))
            forEachLeaf(*tab, [&](vtworkspace::Pane const& leaf) { sessions.push_back(leaf.session()); });
        for (auto const session: sessions)
            closePane(session);
        return vtcontrol::EmptyReply {};
    }));
}

void ControlService::handle(vtcontrol::CallContext& /*context*/,
                            vtcontrol::CreateWorkspaceRequest const& request,
                            vtcontrol::Completion<vtcontrol::CreateWorkspaceReply> done)
{
    // Through the host's own path, which names the workspace after the folder its shell starts in.
    auto* workspace = _host.createWorkspace(spawnRequestOf({}, request.cwd));
    if (workspace == nullptr)
    {
        done(std::unexpected(failed("the new workspace's shell could not be started")));
        return;
    }
    auto const uuid = _host.objects().uuidOf(workspace->id()).value_or(vtcontrol::Uuid {});
    // A label stated outright replaces the default one.
    if (!request.label.empty())
        _host.objects().setLabel(ObjectRef { .kind = ObjectKind::Workspace, .uuid = uuid }, request.label);
    auto const* tab = workspace->activeTab();
    done(vtcontrol::CreateWorkspaceReply {
        .workspace = idOf(ObjectKind::Workspace, uuid),
        .tab = idOf(ObjectKind::Tab, _host.objects().uuidOf(tab->id()).value_or(vtcontrol::Uuid {})),
        .pane = idOf(ObjectKind::Pane,
                     _host.objects().uuidOf(tab->rootPane()->session()).value_or(vtcontrol::Uuid {})),
    });
}

void ControlService::handle(vtcontrol::CallContext& context,
                            vtcontrol::RenameWorkspaceRequest const& request,
                            vtcontrol::Completion<vtcontrol::EmptyReply> done)
{
    // object.setLabel on a workspace, which publishes workspace.labelChanged and tells every window.
    done(resolveWorkspace(context, request.workspace).transform([&](WorkspaceId workspace) {
        _host.objects().setLabel(
            ObjectRef { .kind = ObjectKind::Workspace,
                        .uuid = _host.objects().uuidOf(workspace).value_or(vtcontrol::Uuid {}) },
            request.label);
        return vtcontrol::EmptyReply {};
    }));
}

namespace
{
    /// @return How the control API reports @p error.
    [[nodiscard]] ControlError controlErrorOf(WorkspaceCloseError error)
    {
        auto const code = [error] {
            switch (error)
            {
                case WorkspaceCloseError::UnknownWorkspace: return ControlErrorCode::NotFound;
                case WorkspaceCloseError::LastWorkspace: return ControlErrorCode::Unsupported;
            }
            return ControlErrorCode::Failed;
        }();
        return ControlError { .code = code, .message = std::string { describe(error) }, .candidates = {} };
    }
} // namespace

void ControlService::handle(vtcontrol::CallContext& context,
                            vtcontrol::CloseWorkspaceRequest const& request,
                            vtcontrol::Completion<vtcontrol::EmptyReply> done)
{
    // In one step, as a window's close is: what the workspace holds now ends, and the workspace goes. The
    // only one is not closed.
    done(resolveWorkspace(context, request.workspace).and_then([&](WorkspaceId workspace) {
        return _host.closeWorkspace(workspace)
            .transform([] { return vtcontrol::EmptyReply {}; })
            .transform_error(controlErrorOf);
    }));
}

namespace
{
    /// @param position A position as the API counts them, from 1.
    /// @return The model's 0-based index for it, or BadRequest for 0.
    [[nodiscard]] std::expected<int, ControlError> indexOfPosition(uint32_t position)
    {
        if (position == 0)
            return std::unexpected(ControlError { .code = ControlErrorCode::BadRequest,
                                                  .message = "positions count from 1",
                                                  .candidates = {} });
        return static_cast<int>(std::min<uint32_t>(position - 1, std::numeric_limits<int>::max()));
    }
} // namespace

void ControlService::handle(vtcontrol::CallContext& context,
                            vtcontrol::MoveWorkspaceRequest const& request,
                            vtcontrol::Completion<vtcontrol::EmptyReply> done)
{
    done(indexOfPosition(request.toPosition).and_then([&](int index) {
        return resolveWorkspace(context, request.workspace).transform([&](WorkspaceId workspace) {
            // Clamped by the model, which tells every window the new order.
            _host.model().moveWorkspace(workspace, index);
            return vtcontrol::EmptyReply {};
        });
    }));
}

void ControlService::handle(vtcontrol::CallContext& context,
                            vtcontrol::MoveTabRequest const& request,
                            vtcontrol::Completion<vtcontrol::EmptyReply> done)
{
    done(indexOfPosition(request.toPosition).and_then([&](int index) {
        return resolveTab(context, request.tab).and_then([&](TabId tab) {
            return resolveWorkspace(context, request.toWorkspace).transform([&](WorkspaceId target) {
                auto const source = _host.model().workspaceOfTab(tab);
                _host.model().moveTabToWorkspace(source, tab, target, index);
                // A workspace that lost its last tab goes, as a move over the native protocol leaves none.
                if (auto const* emptied = _host.model().workspace(source);
                    emptied != nullptr && emptied->empty() && source != target)
                    _host.discardWorkspace(source);
                return vtcontrol::EmptyReply {};
            });
        });
    }));
}

namespace
{
    /// @return The control API's error for a move the host refused.
    [[nodiscard]] ControlError moveRefused(NodeMoveFailure const& failure)
    {
        auto const why = failure.model ? vtworkspace::describe(*failure.model) : describe(failure.error);
        return ControlError { .code = ControlErrorCode::Failed,
                              .message = std::format("the pane cannot move there: {}", why),
                              .candidates = {} };
    }
} // namespace

std::expected<vtworkspace::MoveTarget, ControlError> ControlService::paneDestination(
    vtcontrol::CallContext const& context, vtcontrol::MovePaneRequest const& request) const
{
    using vtcontrol::PaneDestination;
    switch (request.to)
    {
        case PaneDestination::Tab:
            return resolveTab(context, request.toTab).transform([&](TabId tab) -> vtworkspace::MoveTarget {
                return vtworkspace::TabTarget { .workspace = _host.model().workspaceOfTab(tab), .tab = tab };
            });
        case PaneDestination::Workspace: {
            // From 1, as tab.move counts, and clamped by the model; none is the end.
            auto const index = request.toPosition
                                   ? indexOfPosition(*request.toPosition)
                                   : std::expected<int, ControlError> { std::numeric_limits<int>::max() };
            return index.and_then([&](int at) {
                return resolveWorkspace(context, request.toWorkspace)
                    .transform([&](WorkspaceId workspace) -> vtworkspace::MoveTarget {
                        return vtworkspace::TabWellTarget { .workspace = workspace, .index = at };
                    });
            });
        }
        case PaneDestination::NewWorkspace: return vtworkspace::NewWorkspaceTarget {};
    }
    return std::unexpected(ControlError {
        .code = ControlErrorCode::BadRequest, .message = "unknown destination", .candidates = {} });
}

void ControlService::handle(vtcontrol::CallContext& context,
                            vtcontrol::MovePaneRequest const& request,
                            vtcontrol::Completion<vtcontrol::MovePaneReply> done)
{
    done(resolvePane(context, request.pane).and_then([&](PaneTarget const& pane) {
        return paneDestination(context, request).and_then([&](vtworkspace::MoveTarget const& target) {
            // The same move a client's MoveNode makes: a workspace it empties goes, a new one is the host's.
            auto const hooks = MoveWorkspaceHooks {
                .create = [this] { return _host.createEmptyWorkspace().id(); },
                .discard = [this](WorkspaceId workspace) { _host.discardWorkspace(workspace); }
            };
            return applyMove(_host.model(), vtworkspace::PaneSource { pane.session }, target, hooks)
                .transform_error(moveRefused)
                .transform([&](vtworkspace::MoveResult const& moved) {
                    auto const& objects = _host.objects();
                    return vtcontrol::MovePaneReply {
                        .workspace = idOf(ObjectKind::Workspace,
                                          objects.uuidOf(moved.workspace).value_or(vtcontrol::Uuid {})),
                        .tab = idOf(ObjectKind::Tab, objects.uuidOf(moved.tab).value_or(vtcontrol::Uuid {})),
                    };
                });
        });
    }));
}

namespace
{
    /// How the control API names a window's answer to a switch.
    struct SwitchOutcomeName
    {
        proto::SwitchOutcome outcome;
        std::string_view name;
    };

    constexpr auto SwitchOutcomeNames = std::array {
        SwitchOutcomeName { proto::SwitchOutcome::Switched, "switched" },
        SwitchOutcomeName { proto::SwitchOutcome::FocusedExisting, "focused" },
        SwitchOutcomeName { proto::SwitchOutcome::PickerShown, "picker" },
        SwitchOutcomeName { proto::SwitchOutcome::Refused, "refused" },
        SwitchOutcomeName { proto::SwitchOutcome::Failed, "failed" },
    };

    /// @param reply A window's answer.
    /// @return The reply the control API gives for it.
    [[nodiscard]] vtcontrol::SwitchWorkspaceReply switchReplyOf(proto::SwitchReply const& reply)
    {
        auto const named = std::ranges::find(SwitchOutcomeNames, reply.outcome, &SwitchOutcomeName::outcome);
        return vtcontrol::SwitchWorkspaceReply {
            .outcome = std::string { named != SwitchOutcomeNames.end() ? named->name : "failed" },
            .detail = reply.detail
        };
    }

    /// @param request A switch request.
    /// @return Where it asks the window to go.
    [[nodiscard]] proto::SwitchTargetWire switchTargetOf(vtcontrol::SwitchWorkspaceRequest const& request)
    {
        auto target = proto::SwitchTargetWire { .workspace = request.toWorkspace };
        if (!request.toLabel.empty())
            target = proto::SwitchTargetWire { .kind = proto::SwitchDaemonKind::Label,
                                               .daemon = request.toLabel,
                                               .workspace = request.toWorkspace };
        else if (!request.toHost.empty())
            target = proto::SwitchTargetWire { .kind = proto::SwitchDaemonKind::Host,
                                               .daemon = request.toHost,
                                               .workspace = request.toWorkspace };
        return target;
    }
} // namespace

void ControlService::handle(vtcontrol::CallContext& context,
                            vtcontrol::SwitchWorkspaceRequest const& request,
                            vtcontrol::Completion<vtcontrol::SwitchWorkspaceReply> done)
{
    if (!request.toLabel.empty() && !request.toHost.empty())
    {
        done(std::unexpected(ControlError { .code = ControlErrorCode::BadRequest,
                                            .message = "name either toLabel or toHost, not both",
                                            .candidates = {} }));
        return;
    }
    // The workspace whose window is asked: the one named, else the caller's pane's; a caller outside every
    // pane names none, and the raise holder answers.
    auto asked = std::optional<WorkspaceId> {};
    if (request.workspace && !request.workspace->empty())
    {
        auto const named = resolveWorkspace(context, *request.workspace);
        if (!named)
        {
            done(std::unexpected(named.error()));
            return;
        }
        asked = *named;
    }
    else if (!context.caller().empty())
    {
        auto const callers = resolveWorkspace(context, {});
        if (!callers)
        {
            done(std::unexpected(callers.error()));
            return;
        }
        asked = *callers;
    }
    auto const views = _host.connectionViews();
    auto const named = request.workspace && !request.workspace->empty();
    auto chosen =
        vthost::switchTarget(views, asked ? std::optional<uint64_t> { asked->value } : std::nullopt);
    // A caller's pane in a workspace nobody selects falls back to the raise holder; only a workspace the
    // request names must be selected by someone.
    if (!chosen && !named)
        chosen = vthost::raiseHolder(views);
    if (!chosen)
    {
        done(std::unexpected(
            notFound(named ? std::format("no window selects workspace '{}'", *request.workspace)
                           : std::string { "no window is attached" })));
        return;
    }
    auto const selection = std::ranges::find(views, *chosen, &vthost::ConnectionView::connection)->selected;

    auto const ticket = ++_switchCalls->lastTicket;
    _switchCalls->waiting.emplace(ticket, &context.events());
    _host.requestSwitch(
        *chosen,
        WorkspaceId { selection },
        switchTargetOf(request),
        [calls = _switchCalls, ticket, done = std::move(done)](proto::SwitchReply const& reply) {
            if (calls->waiting.erase(ticket) != 0)
                done(switchReplyOf(reply));
        });
}

void ControlService::closePane(SessionId session)
{
    // The one place the API ends a session: a requested close, so the pane is not restored.
    _host.closeSession(session);
}

std::expected<vtcontrol::CreateTabReply, ControlError> ControlService::createTab(
    vtcontrol::CallContext const& context, vtcontrol::CreateTabRequest const& request)
{
    auto spawn = spawnRequestOf(request.command, request.cwd);
    auto* tab = static_cast<vtworkspace::Tab*>(nullptr);
    if (request.placement == vtcontrol::WorkspacePlacement::New)
    {
        auto* workspace = _host.createWorkspace(spawn);
        if (workspace == nullptr)
            return std::unexpected(failed("the new workspace's shell could not be started"));
        tab = workspace->activeTab();
    }
    else
    {
        auto const workspaceId = resolveWorkspace(context, request.workspace);
        if (!workspaceId)
            return std::unexpected(workspaceId.error());
        auto* workspace = _host.model().workspace(*workspaceId);
        auto const* previous = workspace->activeTab();
        // A caller inside a pane opens its tab where that pane is; a new workspace inherits nothing.
        if (!context.caller().empty())
            if (auto const caller = resolvePane(context, {}))
                spawn.directoryFrom = caller->session;
        // The host names a workspace by a session it holds.
        auto beside = std::optional<SessionId> {};
        if (previous != nullptr)
            beside = previous->rootPane()->firstLeaf()->session();
        tab = _host.createTab(spawn, beside);
        if (tab == nullptr)
            return std::unexpected(failed("the new tab's shell could not be started"));
        if (request.focus == vtcontrol::FocusPolicy::Keep && previous != nullptr)
            _host.model().activateTab(*workspaceId, previous->id());
    }

    auto const session = tab->rootPane()->session();
    auto const workspaceId = _host.model().workspaceOfTab(tab->id());
    auto const tabRef = ObjectRef { .kind = ObjectKind::Tab, .uuid = *_host.objects().uuidOf(tab->id()) };
    if (!request.label.empty())
        _host.objects().setLabel(tabRef, request.label);
    return vtcontrol::CreateTabReply {
        .workspace =
            idOf(ObjectKind::Workspace, _host.objects().uuidOf(workspaceId).value_or(vtcontrol::Uuid {})),
        .tab = vtcontrol::toString(tabRef),
        .pane = idOf(ObjectKind::Pane, *_host.objects().uuidOf(session)),
    };
}

std::expected<vtcontrol::SplitReply, ControlError> ControlService::split(
    vtcontrol::CallContext const& context, vtcontrol::SplitRequest const& request)
{
    auto const percent = request.ratioPercent.value_or(50);
    if (percent < MinimumSplitPercent || percent > MaximumSplitPercent)
        return std::unexpected(ControlError {
            .code = ControlErrorCode::BadRequest,
            .message = std::format(
                "ratioPercent {} is outside {}..{}", percent, MinimumSplitPercent, MaximumSplitPercent),
            .candidates = {} });
    auto const anchor = resolvePane(context, request.pane);
    if (!anchor)
        return std::unexpected(anchor.error());

    auto const [workspace, tab, leaf] = _host.model().findSessionLeaf(anchor->session);
    if (tab == nullptr)
        return std::unexpected(notFound("the pane is in no tab"));
    auto const previous = tab->activePane()->id();
    auto const paneCount = tab->paneCount();
    _host.model().setActivePane(tab->id(), leaf->id());
    // The new pane starts where its anchor is, unless the caller named a directory itself.
    auto spawn = spawnRequestOf(request.command, request.cwd);
    spawn.directoryFrom = anchor->session;
    // The ratio is the first child's share, and the new pane is the second.
    _host.splitActivePane(tab->id(),
                          request.direction == vtcontrol::SplitDirection::Right
                              ? vtworkspace::SplitState::Vertical
                              : vtworkspace::SplitState::Horizontal,
                          1.0 - (static_cast<double>(percent) / 100.0),
                          spawn);
    if (tab->paneCount() == paneCount)
        return std::unexpected(failed("the new pane's shell could not be started"));

    auto const created = tab->activePane()->session();
    auto const paneRef = ObjectRef { .kind = ObjectKind::Pane, .uuid = *_host.objects().uuidOf(created) };
    if (!request.label.empty())
        _host.objects().setLabel(paneRef, request.label);
    if (request.focus == vtcontrol::FocusPolicy::Keep)
        _host.model().setActivePane(tab->id(), previous);
    return vtcontrol::SplitReply {
        .tab = idOf(ObjectKind::Tab, _host.objects().uuidOf(tab->id()).value_or(vtcontrol::Uuid {})),
        .pane = vtcontrol::toString(paneRef),
    };
}

// }}}
// {{{ Events and waits

void ControlService::handle(vtcontrol::CallContext& context,
                            vtcontrol::WaitRequest const& request,
                            vtcontrol::Completion<vtcontrol::WaitReply> done)
{
    waitForBlock(context, request, std::move(done));
}

void ControlService::waitForBlock(vtcontrol::CallContext& context,
                                  vtcontrol::WaitRequest const& request,
                                  vtcontrol::Completion<vtcontrol::WaitReply> done)
{
    auto const target = resolveBlock(context, request.block, request.pane);
    if (!target)
    {
        done(std::unexpected(target.error()));
        return;
    }
    // Decided under the terminal's lock, answered after it: a reply is encoded and queued, which the parser
    // thread contending for the lock need not wait for. A block that ends meanwhile is still waited for, as
    // its end reaches the loop only after this call.
    auto const pane = target->pane.uuid;
    auto decision =
        [&]() -> std::variant<vtcontrol::BlockRef, std::expected<vtcontrol::WaitReply, ControlError>> {
        auto const* terminal = _host.terminal(target->pane.session);
        auto const guard = std::lock_guard { std::as_const(*terminal) };
        auto const& store = terminal->commandBlocks();

        // A block: at once once it has ended, else when it does.
        if (target->serial)
        {
            auto const* record = findBlock(store, *target->serial);
            if (record == nullptr)
                return std::unexpected(notFound(std::format("no block '{}'", request.block)));
            if (record->state == vtbackend::CommandBlockState::Finished)
                return vtcontrol::WaitReply { .block = blockInfoOf(pane, *record, store.idEpoch()) };
            return vtcontrol::BlockRef { .pane = pane, .serial = *target->serial };
        }

        // A pane: its running command, else at once with its last finished block, or with nothing.
        auto const* current = store.current();
        if (current != nullptr && current->state == vtbackend::CommandBlockState::Running)
            return vtcontrol::BlockRef { .pane = pane,
                                         .serial =
                                             vtcontrol::blockSerial(store.idEpoch(), current->id.value) };
        auto reply = vtcontrol::WaitReply {};
        if (auto const* last = store.lastFinished())
            reply.block = blockInfoOf(pane, *last, store.idEpoch());
        return reply;
    }();

    if (auto* block = std::get_if<vtcontrol::BlockRef>(&decision))
        // A refused wait is answered by the waiters themselves.
        std::ignore = _waiters.waitFor(context.events(), *block, replyingWith(std::move(done)));
    else
        done(std::move(std::get<std::expected<vtcontrol::WaitReply, ControlError>>(decision)));
}

void ControlService::sendAndWait(vtcontrol::CallContext& context,
                                 PaneTarget const& pane,
                                 std::string_view text,
                                 vtcontrol::Completion<vtcontrol::SendReply> done)
{
    // Decided before the text is typed, which is what starts the command: the block at its prompt now is
    // the one the command runs in; with none at its prompt, the next one minted is. Read under the lock,
    // waited for after it, as waitForBlock() does.
    auto const [prompting, serial] = [&] {
        auto const* terminal = _host.terminal(pane.session);
        auto const guard = std::lock_guard { std::as_const(*terminal) };
        auto const& store = terminal->commandBlocks();
        auto const* current = store.current();
        if (current != nullptr && current->state == vtbackend::CommandBlockState::Prompting)
            return std::pair { true, vtcontrol::blockSerial(store.idEpoch(), current->id.value) };
        return std::pair { false, vtcontrol::blockSerial(store.idEpoch(), store.currentId().value) };
    }();
    auto const waiting =
        prompting ? _waiters.waitFor(context.events(),
                                     vtcontrol::BlockRef { .pane = pane.uuid, .serial = serial },
                                     replyingWith(std::move(done)))
                  : _waiters.waitForNext(context.events(), pane.uuid, serial, replyingWith(std::move(done)));
    // Refused, and answered so: nothing is typed that nobody waits for.
    if (!waiting)
        return;
    if (!text.empty())
        _host.writeInput(pane.session, text);
}

void ControlService::handle(vtcontrol::CallContext& context,
                            vtcontrol::SubscribeRequest const& request,
                            vtcontrol::Completion<vtcontrol::SubscribeReply> done)
{
    // A subscription takes only the kinds the grants allow: a `read` token is not handed the command lines
    // `block.finished` carries, which `block.list` keeps behind `screen`.
    auto const grants = context.grants();
    auto kinds = std::vector<std::string> {};
    for (auto const& kind: request.kinds)
    {
        auto const permission = vtcontrol::eventPermission(kind);
        if (!permission)
        {
            done(std::unexpected(ControlError { .code = ControlErrorCode::BadRequest,
                                                .message = std::format("no event is named '{}'", kind),
                                                .candidates = {} }));
            return;
        }
        if (grants.contains(*permission))
            kinds.push_back(kind);
    }
    if (!request.kinds.empty() && kinds.empty())
    {
        done(std::unexpected(ControlError {
            .code = ControlErrorCode::Forbidden,
            .message = std::format("event '{}' needs grant '{}'",
                                   request.kinds.front(),
                                   vtcontrol::nameOf(*vtcontrol::eventPermission(request.kinds.front()))),
            .candidates = {} }));
        return;
    }
    if (request.kinds.empty())
    {
        // Every kind, as far as the grants go. An empty list means every kind, so it stays empty only when
        // the grants allow them all.
        auto const allowed = std::apply(
            [&](auto const&... rows) {
                auto names = std::vector<std::string> {};
                ((grants.contains(rows.permission) ? (void) names.emplace_back(rows.name) : (void) 0), ...);
                return names;
            },
            vtcontrol::EventCatalog);
        if (allowed.empty())
        {
            done(std::unexpected(ControlError { .code = ControlErrorCode::Forbidden,
                                                .message = "no event is granted to this connection",
                                                .candidates = {} }));
            return;
        }
        if (allowed.size() != std::tuple_size_v<decltype(vtcontrol::EventCatalog)>)
            kinds = allowed;
    }
    auto filter = EventFilter { .kinds = std::move(kinds), .pane = std::nullopt };
    if (!request.pane.empty())
    {
        auto const pane = resolvePane(context, request.pane);
        if (!pane)
        {
            done(std::unexpected(pane.error()));
            return;
        }
        filter.pane = pane->uuid;
    }
    done(_hub.subscribe(context.events(), std::move(filter)).transform([](uint32_t subscription) {
        return vtcontrol::SubscribeReply { .subscription = subscription };
    }));
}

void ControlService::handle(vtcontrol::CallContext& context,
                            vtcontrol::UnsubscribeRequest const& request,
                            vtcontrol::Completion<vtcontrol::EmptyReply> done)
{
    done(_hub.unsubscribe(context.events(), request.subscription).transform([] {
        return vtcontrol::EmptyReply {};
    }));
}

void ControlService::clientAttached(std::string_view client)
{
    _hub.publish(vtcontrol::Event {
        .origin = {}, .payload = vtcontrol::ClientAttachedEvent { .client = std::string { client } } });
}

void ControlService::clientDetached(std::string_view client)
{
    _hub.publish(vtcontrol::Event {
        .origin = {}, .payload = vtcontrol::ClientDetachedEvent { .client = std::string { client } } });
}

void ControlService::sinkClosed(vtcontrol::EventSink& sink)
{
    _hub.forget(sink);
    _waiters.forget(sink);
    std::erase_if(_switchCalls->waiting, [&sink](auto const& call) { return call.second == &sink; });
}

void ControlService::labelChanged(ObjectRef const& ref, std::string_view label)
{
    if (ref.kind != ObjectKind::Workspace)
        return;
    _hub.publish(vtcontrol::Event {
        .origin = { .workspace = idOf(ObjectKind::Workspace, ref.uuid), .tab = {}, .pane = {} },
        .payload = vtcontrol::WorkspaceLabelChangedEvent { .label = vtcontrol::toValidUtf8(label) } });
}

vtcontrol::EventOrigin ControlService::originOf(SessionId session) const
{
    auto origin = vtcontrol::EventOrigin {};
    if (auto const uuid = _host.objects().uuidOf(session))
        origin.pane = idOf(ObjectKind::Pane, *uuid);
    if (auto const [workspace, tab, leaf] = _host.model().findSessionLeaf(session); tab != nullptr)
    {
        auto const owner = originOf(workspace->id(), tab->id());
        origin.workspace = owner.workspace;
        origin.tab = owner.tab;
    }
    return origin;
}

vtcontrol::EventOrigin ControlService::originOf(WorkspaceId workspace, TabId tab) const
{
    auto origin = vtcontrol::EventOrigin {};
    if (auto const uuid = _host.objects().uuidOf(workspace))
        origin.workspace = idOf(ObjectKind::Workspace, *uuid);
    if (auto const uuid = _host.objects().uuidOf(tab))
        origin.tab = idOf(ObjectKind::Tab, *uuid);
    return origin;
}

std::optional<vtcontrol::BlockInfo> ControlService::blockInfoFor(SessionId session,
                                                                 vtbackend::CommandBlockId block) const
{
    auto const uuid = _host.objects().uuidOf(session);
    auto const* terminal = _host.terminal(session);
    if (!uuid || terminal == nullptr)
        return std::nullopt;
    auto const guard = std::lock_guard { std::as_const(*terminal) };
    auto const& store = terminal->commandBlocks();
    if (auto const* record = store.find(block))
        return blockInfoOf(*uuid, *record, store.idEpoch());
    return std::nullopt;
}

// The source-to-event mapping: each host event below is one row, and so is each SessionSignal alternative in
// sessionSignal().

void ControlService::sessionSignal(SessionId session, SessionSignal const& signal)
{
    std::visit(core::Overloaded {
                   [&](CommandBlockFinishedSignal const& finished) {
                       // The record as it reads now; the summary when it is no longer kept, which says as
                       // much.
                       auto info = blockInfoFor(session, finished.summary.id);
                       if (!info)
                       {
                           auto const* terminal = _host.terminal(session);
                           auto record = vtbackend::CommandBlockRecord {};
                           record.id = finished.summary.id;
                           record.state = vtbackend::CommandBlockState::Finished;
                           record.commandLine = finished.summary.commandLine;
                           record.exitCode = finished.summary.exitCode;
                           record.duration = finished.summary.duration;
                           record.workingDirectory = finished.summary.workingDirectory;
                           auto const epoch = [&] {
                               if (terminal == nullptr)
                                   return uint64_t { 0 };
                               auto const guard = std::lock_guard { std::as_const(*terminal) };
                               return terminal->commandBlocks().idEpoch();
                           }();
                           info = blockInfoOf(
                               _host.objects().uuidOf(session).value_or(vtcontrol::Uuid {}), record, epoch);
                       }
                       _hub.publish(
                           vtcontrol::Event { .origin = originOf(session),
                                              .payload = vtcontrol::BlockFinishedEvent { .block = *info } });
                       _waiters.blockEnded(*info);
                   },
                   [&](CommandBlockChangedSignal const& changed) {
                       // A block a newer prompt closed was never reported finished: this is its end.
                       if (auto const info = blockInfoFor(session, changed.id);
                           info && info->state == vtcontrol::BlockState::Superseded)
                           _waiters.blockEnded(*info);
                   },
                   [&](TitleChangedSignal const& title) {
                       _hub.publish(vtcontrol::Event { .origin = originOf(session),
                                                       .payload = vtcontrol::TitleChangedEvent {
                                                           .title = vtcontrol::toValidUtf8(title.title) } });
                   },
                   [&](WorkingDirectoryChangedSignal const&) {
                       auto cwd = std::string {};
                       if (auto const* terminal = _host.terminal(session))
                       {
                           auto const guard = std::lock_guard { std::as_const(*terminal) };
                           if (auto const resolved =
                                   terminal->resolveWorkingDirectory(vtbackend::CwdPurpose::Display))
                               cwd = vtcontrol::toValidUtf8(resolved->path);
                       }
                       _hub.publish(vtcontrol::Event {
                           .origin = originOf(session),
                           .payload = vtcontrol::CwdChangedEvent { .cwd = std::move(cwd) } });
                   },
                   [&](ProgramStatusChangedSignal const& changed) {
                       // The whole set as it reads now: a subscriber holds what a pane says, not a diff.
                       auto records = std::vector<vtcontrol::ProgramStatusInfo> {};
                       if (auto const* terminal = _host.terminal(session))
                       {
                           auto const guard = std::lock_guard { std::as_const(*terminal) };
                           records = toApi(terminal->programStatus());
                       }
                       _hub.publish(vtcontrol::Event {
                           .origin = originOf(session),
                           .payload = vtcontrol::ProgramStatusChangedEvent {
                               .records = std::move(records),
                               .reason = toApi(ProgramStatusReasonBridge, changed.reason) } });
                   },
               },
               signal);
}

void ControlService::sessionBell(SessionId session)
{
    _hub.publish(vtcontrol::Event { .origin = originOf(session), .payload = vtcontrol::BellEvent {} });
}

void ControlService::sessionClosing(SessionId session)
{
    _closing = ClosingPane { .session = session, .origin = originOf(session) };
}

void ControlService::sessionClosed(SessionId session)
{
    // The pane has left the model already, so its tab and workspace are what sessionClosing() saw; its UUID
    // is forgotten only after this.
    auto const uuid = _host.objects().uuidOf(session);
    auto origin = _closing && _closing->session == session ? std::move(_closing->origin) : originOf(session);
    _closing.reset();
    _hub.publish(vtcontrol::Event { .origin = std::move(origin), .payload = vtcontrol::PaneClosedEvent {} });
    if (uuid)
        _waiters.paneClosed(*uuid);
}

void ControlService::tabAdded(WorkspaceId workspace, TabId tab, int /*index*/)
{
    auto origin = originOf(workspace, tab);
    _hub.publish(vtcontrol::Event { .origin = origin, .payload = vtcontrol::TabCreatedEvent {} });
    if (auto const* created = _host.model().findTab(tab))
        if (auto const uuid = _host.objects().uuidOf(created->rootPane()->session()))
        {
            origin.pane = idOf(ObjectKind::Pane, *uuid);
            _hub.publish(
                vtcontrol::Event { .origin = std::move(origin), .payload = vtcontrol::PaneCreatedEvent {} });
        }
}

void ControlService::paneSplit(TabId tab, vtworkspace::PaneId /*splitNode*/, vtworkspace::PaneId newLeaf)
{
    if (auto const* leaf = _host.model().findPane(tab, newLeaf))
        _hub.publish(vtcontrol::Event { .origin = originOf(leaf->session()),
                                        .payload = vtcontrol::PaneCreatedEvent {} });
}

void ControlService::tabClosed(WorkspaceId workspace, TabId tab, int /*index*/)
{
    _hub.publish(
        vtcontrol::Event { .origin = originOf(workspace, tab), .payload = vtcontrol::TabClosedEvent {} });
}

// }}}

} // namespace vthost::control
