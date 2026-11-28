// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `ScriptedAddressResolver` — a blocking resolver that answers from a script, deterministically,
/// with no name server and no thread of its own.

#include <core/net/NetError.hpp>
#include <core/net/SocketAddress.hpp>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace core::net::testing
{

/// An @c IAddressResolver whose answers a test writes.
///
/// - A **literal** (`127.0.0.1`, `::1`) is parsed by @c detail::endpointOfLiteral, as a real
///   resolver parses it, and never answered from the script: a dial to one tests what production does.
///   Only the strict forms are literals; `127.1`, which `getaddrinfo` expands, is a name here and must
///   be scripted by a test that dials it.
/// - A **name** given to @c answer resolves to its endpoints, verbatim.
/// - Any other name fails as the last of @c failWith or @c failWithCode said, and by default as
///   @c NetErrorCode::NameNotFound, which is what a real resolver says of a name it does not know.
///
/// @c failWith scripts a failure in words only, so @c resolveClassified reports it through the
/// default classification of @c IAddressResolver -- the double for a resolver written before that
/// seam existed. @c failWithCode scripts the classified failure itself.
///
/// **It owns no thread.** Every lookup runs on the caller's. Its state is locked, so a resolver that
/// offloads lookups (@c ThreadedAddressResolver) may call it from its own threads; @c hold is for
/// exactly that case, since a held lookup blocks the thread that called it until @c release.
class ScriptedAddressResolver final: public IAddressResolver
{
  public:
    /// Scripts @p host to resolve to @p endpoints.
    /// @param host The name, as a lookup will be asked for it.
    /// @param endpoints The answer, in preference order. Their ports are not rewritten.
    void answer(std::string host, std::vector<ResolvedEndpoint> endpoints)
    {
        auto const guard = std::scoped_lock { _mutex };
        _answers.insert_or_assign(std::move(host), std::move(endpoints));
    }

    /// Fails every unscripted name with @p message, unclassified.
    /// @param message What @c resolve reports, and the context of the default classification.
    void failWith(std::string message)
    {
        auto const guard = std::scoped_lock { _mutex };
        _failure = std::move(message);
    }

    /// Fails every unscripted name with @p error.
    /// @param error What @c resolveClassified reports; @c resolve reports it as words.
    void failWithCode(NetError error)
    {
        auto const guard = std::scoped_lock { _mutex };
        _failure = std::move(error);
    }

    /// Makes every later lookup block inside the resolver, on the calling thread, until @c release.
    void hold()
    {
        auto const guard = std::scoped_lock { _mutex };
        _isHeld = true;
    }

    /// Lets every held lookup, and every later one, return.
    void release()
    {
        {
            auto const guard = std::scoped_lock { _mutex };
            _isHeld = false;
        }
        _released.notify_all();
    }

    /// Waits, at most @p budget, until @p count lookups have entered the resolver.
    /// @param count How many lookups to wait for, counted since construction.
    /// @param budget The longest this waits.
    /// @return How many lookups had entered when the wait ended: @p count or more, or fewer when
    ///         the budget ran out.
    [[nodiscard]] std::size_t awaitEntered(std::size_t count, std::chrono::milliseconds budget) const
    {
        auto lock = std::unique_lock { _mutex };
        _entered.wait_for(lock, budget, [&] { return _calls >= count; });
        return _calls;
    }

    /// @return How many lookups were asked of this resolver, literals included.
    [[nodiscard]] std::size_t calls() const
    {
        auto const guard = std::scoped_lock { _mutex };
        return _calls;
    }

    /// @return The thread each lookup ran on, in call order, literals included -- so a test can
    ///         say which thread paid for a lookup an async resolver offloaded.
    [[nodiscard]] std::vector<std::thread::id> callers() const
    {
        auto const guard = std::scoped_lock { _mutex };
        return _callers;
    }

    /// @copydoc IAddressResolver::resolve
    [[nodiscard]] std::expected<std::vector<ResolvedEndpoint>, std::string> resolve(
        std::string_view host, std::uint16_t port) override
    {
        auto outcome = lookup(host, port);
        if (auto* const endpoints = std::get_if<std::vector<ResolvedEndpoint>>(&outcome))
            return std::move(*endpoints);
        if (auto* const message = std::get_if<std::string>(&outcome))
            return std::unexpected(std::move(*message));
        return std::unexpected(std::get<NetError>(outcome).toString());
    }

    /// @copydoc IAddressResolver::resolveClassified
    ///
    /// A failure scripted by @c failWith takes the default classification.
    [[nodiscard]] std::expected<std::vector<ResolvedEndpoint>, NetError> resolveClassified(
        std::string_view host, std::uint16_t port) override
    {
        auto outcome = lookup(host, port);
        if (auto* const endpoints = std::get_if<std::vector<ResolvedEndpoint>>(&outcome))
            return std::move(*endpoints);
        if (auto* const error = std::get_if<NetError>(&outcome))
            return std::unexpected(std::move(*error));
        auto message = std::move(std::get<std::string>(outcome));
        return Unclassified { std::move(message) }.resolveClassified(host, port);
    }

  private:
    /// What one lookup came to: endpoints, a worded failure, or a classified one.
    using Outcome = std::variant<std::vector<ResolvedEndpoint>, std::string, NetError>;

    /// A resolver that knows only words, so that @c resolveClassified reaches the default
    /// classification through the interface rather than restating it.
    class Unclassified final: public IAddressResolver
    {
      public:
        explicit Unclassified(std::string message) noexcept: _message(std::move(message)) {}

        [[nodiscard]] std::expected<std::vector<ResolvedEndpoint>, std::string> resolve(
            std::string_view /*host*/, std::uint16_t /*port*/) override
        {
            return std::unexpected(_message);
        }

      private:
        std::string _message;
    };

    /// Counts the lookup, waits out a hold, and answers it.
    [[nodiscard]] Outcome lookup(std::string_view host, std::uint16_t port)
    {
        auto lock = std::unique_lock { _mutex };
        ++_calls;
        _callers.push_back(std::this_thread::get_id());
        _entered.notify_all();
        _released.wait(lock, [this] { return !_isHeld; });

        if (auto literal = detail::endpointOfLiteral(host, port))
            return std::vector<ResolvedEndpoint> { *literal };
        if (auto const answer = _answers.find(host); answer != _answers.end())
            return answer->second;
        if (auto const* const message = std::get_if<std::string>(&_failure))
            return *message;
        if (auto const* const error = std::get_if<NetError>(&_failure))
            return *error;
        return makeNetError(NetErrorCode::NameNotFound, 0, std::format("no script answers '{}'", host));
    }

    mutable std::mutex _mutex;
    mutable std::condition_variable _entered;
    std::condition_variable _released;
    std::map<std::string, std::vector<ResolvedEndpoint>, std::less<>> _answers;
    /// The unscripted names' failure: none scripted yet (`std::monostate`, @c NetErrorCode::NameNotFound),
    /// worded, or classified.
    std::variant<std::monostate, std::string, NetError> _failure;
    std::vector<std::thread::id> _callers;
    std::size_t _calls = 0;
    bool _isHeld = false;
};

} // namespace core::net::testing
