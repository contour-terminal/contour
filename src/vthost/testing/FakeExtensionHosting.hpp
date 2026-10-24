// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// An IExtensionHosting that runs no WebAssembly: each session's extensions are a
/// vtbackend::FakeStatusLineExtensions the test publishes frames through, so what a host and its clients do
/// with a frame is testable in a build without extensions too.

#include <vtbackend/grid/Line.hpp>
#include <vtbackend/screen/StatusLineExtensions.hpp>
#include <vtbackend/testing/FakeStatusLineExtensions.hpp>
#include <vtbackend/testing/MockTerm.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vthost/ExtensionHosting.hpp>
#include <vthost/PermissionWire.hpp>

namespace vthost::testing
{

/// @return A frame whose one row is @p vt as a terminal draws it, @p columns wide.
[[nodiscard]] inline std::shared_ptr<vtbackend::StatusLineExtensionSnapshot const> frameOf(
    std::string_view vt, uint64_t generation = 1, int columns = 40)
{
    auto surface = vtbackend::MockTerm { vtbackend::PageSize { vtbackend::LineCount(1),
                                                               vtbackend::ColumnCount(columns) } };
    surface.writeToScreen(vt);
    auto line = surface.terminal.primaryScreen().grid().lineAt(vtbackend::LineOffset(0));
    // As a Surface detaches a frame's images: the surface terminal dies with this function.
    vtbackend::rehomeImageFragments(line, &vtbackend::detachedImage);
    return std::make_shared<vtbackend::StatusLineExtensionSnapshot const>(
        vtbackend::StatusLineExtensionSnapshot {
            .lines = { std::move(line) }, .sgrText = std::string { vt }, .generation = generation });
}

/// One session's extensions, as FakeExtensionHosting makes them.
class FakeSessionExtensions final: public ISessionExtensions
{
  public:
    explicit FakeSessionExtensions(SessionExtensionEvents events): _events { std::move(events) } {}

    [[nodiscard]] vtbackend::StatusLineExtensionServices terminalServices() noexcept override
    {
        return fake.services();
    }

    void start() override { ++starts; }

    [[nodiscard]] vtbackend::StatusLineEventKinds subscriptions() const noexcept override
    {
        return fake.subscriptions();
    }

    /// Publishes @p frame as @p name's, as a worker does: the frame first, then the sink.
    void publish(std::string const& name, std::shared_ptr<vtbackend::StatusLineExtensionSnapshot const> frame)
    {
        fake.publish(name, std::move(frame));
        fake.sink.publish();
    }

    /// Tells the host other extensions run now, as a build that landed does. On the host's loop thread.
    void replaced() const { _events.onReplaced(); }

    /// Asks the host, as a runner whose command was refused as pending does.
    void askPermission(proto::PermissionRequest request) const
    {
        _events.onPermissionNeeded(std::move(request));
    }

    /// Calls a control verb, as a runner whose extension called its import does: from the extension's lane,
    /// which is to say from any thread but the host's loop.
    [[nodiscard]] std::expected<std::string, vtcontrol::ControlError> callControl(
        std::string_view method,
        std::string_view params,
        vtcontrol::ControlGrants granted,
        std::chrono::milliseconds timeout) const
    {
        return _events.callControl(method, params, granted, timeout);
    }

    /// The terminal's view of the extensions: what a test sets the take-over and the frames on.
    vtbackend::FakeStatusLineExtensions fake;
    int starts = 0; ///< How many times start() was called.

  private:
    SessionExtensionEvents _events;
};

/// An IExtensionHosting whose sessions' extensions are FakeSessionExtensions, and whose grant store is a map.
class FakeExtensionHosting final: public IExtensionHosting
{
  public:
    /// Sets up each new session's extensions before its terminal exists: what a test takes over with.
    std::function<void(vtbackend::FakeStatusLineExtensions&)> prepare;

    [[nodiscard]] std::unique_ptr<ISessionExtensions> createSession(SessionExtensionEvents events,
                                                                    OwnerPost /*post*/) override
    {
        auto made = std::make_unique<FakeSessionExtensions>(std::move(events));
        if (prepare)
            prepare(made->fake);
        sessions.push_back(made.get());
        return made;
    }

    void answer(std::string_view hostId, std::string_view digest, GrantState answer) override
    {
        if (answer != GrantState::Pending)
            answers[{ std::string { hostId }, std::string { digest } }] = answer;
        answered.push_back(answer);
    }

    [[nodiscard]] GrantState lookup(std::string_view hostId, std::string_view digest) const override
    {
        auto const found = answers.find({ std::string { hostId }, std::string { digest } });
        return found != answers.end() ? found->second : GrantState::Pending;
    }

    void releaseRetired() noexcept override { ++releases; }

    void stop() noexcept override { ++stops; }

    /// Every session's extensions made, oldest first; a session gone leaves its pointer dangling.
    std::vector<FakeSessionExtensions*> sessions;
    std::map<std::pair<std::string, std::string>, GrantState> answers; ///< What lookup() answers.
    std::vector<GrantState> answered; ///< Every answer() call's answer, in order.
    int releases = 0;                 ///< How many times releaseRetired() was called.
    int stops = 0;                    ///< How many times stop() was called.
};

} // namespace vthost::testing
