// SPDX-License-Identifier: Apache-2.0
//
// The fake's own contract: the scripted failures come first and in order, then the decorated
// listener's accepts; the rest of the interface reaches the decorated listener.
#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/NetError.hpp>
#include <core/net/testing/FailingListener.hpp>
#include <core/net/testing/InMemorySocket.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using core::net::EventLoop;
using core::net::makeNetError;
using core::net::NetErrorCode;
using core::net::testing::FailingListener;

namespace
{

/// The decorated listener: hands out one in-memory connection, then answers `Cancelled`, and never
/// parks. A fake that stopped answering its script would otherwise park on an accept that no
/// connection and no stop token can resume -- an in-memory park hears no stop -- and the case would
/// hang rather than fail.
class OneConnectionListener final: public core::net::IListener
{
  public:
    [[nodiscard]] core::async::Task<core::net::AcceptResult> accept() override
    {
        ++_accepts;
        if (_closed)
            co_return std::unexpected(makeNetError(NetErrorCode::Cancelled, 0, "inner closed"));
        if (_handedOut)
            co_return std::unexpected(makeNetError(NetErrorCode::Cancelled, 0, "inner has no more"));
        _handedOut = true;
        co_return std::unique_ptr<core::net::ISocket> {
            core::net::testing::InMemorySocketPair::create().server
        };
    }

    [[nodiscard]] std::uint16_t boundPort() const noexcept override { return 4242; }

    /// @return How many accepts reached this listener.
    [[nodiscard]] int accepts() const noexcept { return _accepts; }

  protected:
    void doClose() noexcept override { _closed = true; }

  private:
    int _accepts = 0;
    bool _handedOut = false;
    bool _closed = false;
};

/// @param loop The loop to run it on.
/// @param listener The listener to accept from.
/// @return What one accept answered, in words: `accepted`, or the error.
[[nodiscard]] std::string acceptOnce(EventLoop& loop, core::net::IListener& listener)
{
    auto const accepted = loop.blockOn(listener.accept());
    return accepted.has_value() ? std::string { "accepted" } : accepted.error().toString();
}

} // namespace

TEST_CASE("A failing listener answers its script in order, then accepts from the listener it decorates",
          "[net][accept-policy]")
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = EventLoop { *source };
    auto inner = OneConnectionListener {};
    auto failing = FailingListener { inner,
                                     { makeNetError(NetErrorCode::ResourceExhausted, 24, "accept"),
                                       makeNetError(NetErrorCode::ConnReset, 10054, "AcceptEx") } };

    auto answers = std::vector<std::string> {};
    for ([[maybe_unused]] auto const i: { 1, 2, 3 })
        answers.push_back(acceptOnce(loop, failing));
    CHECK(answers
          == std::vector<std::string> { "resource exhausted (accept) [errno 24]",
                                        "connection reset (AcceptEx) [errno 10054]",
                                        "accepted" });
    // The scripted answers never touched the decorated listener.
    CHECK(inner.accepts() == 1);
    CHECK(failing.failuresAnswered() == 2);

    // The rest of the interface reaches the decorated listener.
    CHECK(failing.boundPort() == 4242);
    failing.close();
    CHECK(acceptOnce(loop, failing) == "cancelled (inner closed)");
}

TEST_CASE("A failing listener may own the listener it decorates", "[net][accept-policy]")
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = EventLoop { *source };
    auto owned = std::make_unique<OneConnectionListener>();
    auto const* const inner = owned.get();

    auto failing = FailingListener { std::move(owned),
                                     core::net::testing::repeatedFailures(NetErrorCode::BadHandle, 2) };

    auto codes = std::string {};
    for ([[maybe_unused]] auto const i: { 1, 2, 3 })
        codes += acceptOnce(loop, failing) + "; ";
    CHECK(codes == "bad handle (accept); bad handle (accept); accepted; ");
    CHECK(inner->accepts() == 1);
}

TEST_CASE("A closed failing listener answers no more of its script", "[net][accept-policy]")
{
    // A real listener closed answers `Cancelled`, whatever else it might have said; a fake that
    // went on answering its script after a close would let a loop that misses the close pass.
    auto const source = core::net::makeDefaultBackend();
    auto loop = EventLoop { *source };
    auto inner = OneConnectionListener {};
    auto failing =
        FailingListener { inner, core::net::testing::repeatedFailures(NetErrorCode::ResourceExhausted, 3) };

    CHECK(acceptOnce(loop, failing) == "resource exhausted (accept)");
    failing.close();
    CHECK(failing.closeToken().stop_requested());
    CHECK(acceptOnce(loop, failing) == "cancelled (inner closed)");
    CHECK(failing.failuresAnswered() == 1);
}
