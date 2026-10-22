// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/screen/StatusLineExtensions.hpp>

#include <atomic>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vtbackend
{

/// An IStatusLineExtensions that records what the terminal asks of it. Every member may be touched from
/// two threads, as the production one is: the terminal reads it while a worker publishes.
class FakeStatusLineExtensions final: public IStatusLineExtensions
{
  public:
    /// @param maxUserVars What services() tells the terminal it may remember of user variables.
    explicit FakeStatusLineExtensions(std::size_t maxUserVars = 32): _maxUserVars { maxUserVars } {}

    [[nodiscard]] StatusLineEventKinds subscriptions() const noexcept override
    {
        return StatusLineEventKinds::fromValue(_subscribed.load());
    }

    void post(StatusLineEvent event) noexcept override
    {
        auto const lock = std::scoped_lock { _mutex };
        _posted.push_back(std::move(event));
    }

    void resize(PageSize statusLineSize) noexcept override
    {
        auto const lock = std::scoped_lock { _mutex };
        _resizes.push_back(statusLineSize);
    }

    [[nodiscard]] std::shared_ptr<StatusLineExtensionSnapshot const> snapshot(
        std::string_view name) const noexcept override
    {
        auto const lock = std::scoped_lock { _mutex };
        ++_snapshotCalls[std::string(name)];
        auto const i = _snapshots.find(std::string(name));
        return i != _snapshots.end() ? i->second : nullptr;
    }

    [[nodiscard]] std::optional<std::string> takeOverName() const noexcept override
    {
        auto const lock = std::scoped_lock { _mutex };
        ++_takeOverNameCalls;
        return _takeOver;
    }

    /// @return The services a terminal shows these extensions through.
    [[nodiscard]] StatusLineExtensionServices services()
    {
        return { .extensions = *this, .published = sink, .maxUserVars = _maxUserVars };
    }

    [[nodiscard]] int takeOverNameCalls() const
    {
        auto const lock = std::scoped_lock { _mutex };
        return _takeOverNameCalls;
    }

    /// @return The generation of @p name's latest frame; 0 for none.
    [[nodiscard]] uint64_t generationOf(std::string const& name) const
    {
        auto const lock = std::scoped_lock { _mutex };
        auto const i = _snapshots.find(name);
        return i != _snapshots.end() && i->second ? i->second->generation : 0;
    }

    /// What the workers tell of a published frame through.
    StatusLinePublishSink sink;

    void subscribe(StatusLineEventKinds kinds) { _subscribed.store(kinds.value()); }

    void publish(std::string const& name, std::shared_ptr<StatusLineExtensionSnapshot const> snapshot)
    {
        auto const lock = std::scoped_lock { _mutex };
        _snapshots[name] = std::move(snapshot);
    }

    void takeOver(std::optional<std::string> name)
    {
        auto const lock = std::scoped_lock { _mutex };
        _takeOver = std::move(name);
    }

    [[nodiscard]] std::vector<StatusLineEvent> posted() const
    {
        auto const lock = std::scoped_lock { _mutex };
        return _posted;
    }

    [[nodiscard]] std::vector<PageSize> resizes() const
    {
        auto const lock = std::scoped_lock { _mutex };
        return _resizes;
    }

    [[nodiscard]] int snapshotCalls(std::string const& name) const
    {
        auto const lock = std::scoped_lock { _mutex };
        auto const i = _snapshotCalls.find(name);
        return i != _snapshotCalls.end() ? i->second : 0;
    }

    void forgetSnapshotCalls()
    {
        auto const lock = std::scoped_lock { _mutex };
        _snapshotCalls.clear();
    }

  private:
    std::size_t _maxUserVars;
    std::atomic<StatusLineEventKinds::value_type> _subscribed = 0;
    mutable std::mutex _mutex;
    std::vector<StatusLineEvent> _posted;
    std::vector<PageSize> _resizes;
    std::map<std::string, std::shared_ptr<StatusLineExtensionSnapshot const>> _snapshots;
    mutable std::map<std::string, int> _snapshotCalls;
    std::optional<std::string> _takeOver;
    mutable int _takeOverNameCalls = 0;
};

} // namespace vtbackend
