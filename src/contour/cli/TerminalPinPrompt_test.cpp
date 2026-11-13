// SPDX-License-Identifier: Apache-2.0
#include <contour/cli/TerminalPinPrompt.hpp>

#include <core/async/WhenAll.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/WithTimeout.hpp>
#include <core/net/testing/CoroTestSupport.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <sstream>
#include <string>
#include <string_view>

#ifndef _WIN32
    #include <fcntl.h>
    #include <unistd.h>
#endif

TEST_CASE("an unattended terminal gets no pin prompt", "[contour][cli][pin]")
{
    auto out = std::ostringstream {};
    CHECK(contour::cli::makeTerminalPinPrompt(contour::cli::TerminalAttendance::Unattended, 0, out)
          == nullptr);
    CHECK(contour::cli::makeTerminalPinPrompt(contour::cli::TerminalAttendance::Attended, 0, out) != nullptr);
}

#ifndef _WIN32
namespace
{
constexpr auto HostPort = std::string_view { "studio.example:7297" };
constexpr auto Fingerprint = std::string_view { "SHA256:47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU" };

/// A pipe standing in for the terminal's input: what is written to it is what the user types.
class TypedInput
{
  public:
    TypedInput()
    {
        auto ends = std::array<int, 2> {};
        REQUIRE(::pipe(ends.data()) == 0);
        _read = ends[0];
        _write = ends[1];
    }
    ~TypedInput()
    {
        ::close(_read);
        if (_write >= 0)
            ::close(_write);
    }

    TypedInput(TypedInput const&) = delete;
    TypedInput& operator=(TypedInput const&) = delete;
    TypedInput(TypedInput&&) = delete;
    TypedInput& operator=(TypedInput&&) = delete;

    /// @return What the prompt reads.
    [[nodiscard]] int reading() const noexcept { return _read; }

    /// The user types @p text.
    void type(std::string_view text) const { REQUIRE(::write(_write, text.data(), text.size()) >= 0); }

    /// The input ends: nothing more will be typed.
    void end()
    {
        ::close(_write);
        _write = -1;
    }

    /// @return How many bytes of what was typed are still unread: nonzero only when nobody reads.
    [[nodiscard]] long unread() const
    {
        auto const flags = ::fcntl(_read, F_GETFL);
        ::fcntl(_read, F_SETFL, flags | O_NONBLOCK);
        auto buffer = std::array<char, 16> {};
        auto const count = ::read(_read, buffer.data(), buffer.size());
        ::fcntl(_read, F_SETFL, flags);
        return count;
    }

  private:
    int _read = -1;
    int _write = -1;
};

struct Asked
{
    vthost::PinDecision decision;
    std::string said;
};

[[nodiscard]] Asked askWith(std::string_view typed)
{
    auto input = TypedInput {};
    input.type(typed);
    input.end();
    auto out = std::ostringstream {};
    auto prompt = contour::cli::TerminalPinPrompt { input.reading(), out };
    auto const backend = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *backend };
    auto const decision = loop.blockOn(prompt.confirm(std::string { HostPort }, std::string { Fingerprint }));
    return Asked { .decision = decision, .said = out.str() };
}

core::async::Task<void> typeAfter(core::net::EventLoop* loop,
                                  TypedInput* input,
                                  std::chrono::milliseconds wait,
                                  int* timerTicks)
{
    co_await loop->delay(wait);
    ++*timerTicks; // the loop's timer ran while the prompt waited for the user
    input->type("y\n");
}

core::async::Task<void> confirmInto(contour::cli::TerminalPinPrompt* prompt, vthost::PinDecision* out)
{
    *out = co_await prompt->confirm(std::string { HostPort }, std::string { Fingerprint });
}
} // namespace

TEST_CASE("TerminalPinPrompt shows the daemon and its fingerprint", "[contour][cli][pin]")
{
    auto const asked = askWith("y\n");
    CHECK(asked.said.contains(HostPort));
    CHECK(asked.said.contains(Fingerprint));
    CHECK(asked.said.contains("Compare with `contour daemon status` on that machine."));
    CHECK(asked.said.contains("Trust this daemon? [y/N]"));
}

TEST_CASE("TerminalPinPrompt trusts on y or yes and on nothing else", "[contour][cli][pin]")
{
    auto const trusting = std::array<std::string_view, 5> { "y\n", "yes\n", "Y\n", " YES \n", "yes" };
    for (auto const typed: trusting)
    {
        INFO(typed);
        CHECK(askWith(typed).decision == vthost::PinDecision::Trust);
    }
    auto const declining =
        std::array<std::string_view, 7> { "\n", "n\n", "no\n", "yess\n", "ye\n", "trust\n", "" };
    for (auto const typed: declining)
    {
        INFO(typed);
        CHECK(askWith(typed).decision == vthost::PinDecision::Decline);
    }
}

TEST_CASE("TerminalPinPrompt leaves the loop's timers running while the user types", "[contour][cli][pin]")
{
    using namespace std::chrono_literals;
    auto input = TypedInput {};
    auto out = std::ostringstream {};
    auto prompt = contour::cli::TerminalPinPrompt { input.reading(), out };
    auto const backend = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *backend };
    auto decision = vthost::PinDecision::Decline;
    auto ticks = 0;

    // A read that blocked this loop would never let the timer fire, and the bound would end the wait.
    auto const finished = loop.blockOn(core::net::withTimeout(
        &loop,
        core::net::testing::allOf(confirmInto(&prompt, &decision), typeAfter(&loop, &input, 30ms, &ticks)),
        5s));

    REQUIRE(finished);
    CHECK(ticks == 1);
    CHECK(decision == vthost::PinDecision::Trust);
}

TEST_CASE("a TerminalPinPrompt that is cancelled lets go of the input", "[contour][cli][pin]")
{
    using namespace std::chrono_literals;
    auto input = TypedInput {};
    auto out = std::ostringstream {};
    auto prompt = contour::cli::TerminalPinPrompt { input.reading(), out };
    auto const backend = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *backend };

    auto const answered = loop.blockOn(core::net::withTimeout(
        &loop, prompt.confirm(std::string { HostPort }, std::string { Fingerprint }), 50ms));

    // The wait ended at the bound, though the user typed nothing. Its reader is gone, joined as the wait
    // unwound -- a process that ends now is not held up by a read -- so what is typed later stays unread,
    // for whoever asks next, where a reader left behind would have taken it.
    CHECK_FALSE(answered.has_value());
    input.type("y\n");
    CHECK(input.unread() == 2);
}
#endif
