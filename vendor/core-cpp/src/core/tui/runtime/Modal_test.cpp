// SPDX-License-Identifier: Apache-2.0
#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>
#include <core/platform/SignalHandler.hpp>
#include <core/platform/SystemPipe.hpp>
#include <core/platform/Wakeup.hpp>
#include <core/tui/Canvas.hpp>
#include <core/tui/InputEvent.hpp>
#include <core/tui/runtime/Modal.hpp>
#include <core/tui/runtime/TuiRuntime.hpp>
#include <core/tui/runtime/testing/ScriptedInputSource.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <utility>
#include <variant>

using core::net::EventLoop;
using core::tui::Canvas;
using core::tui::InputEvent;
using core::tui::KeyEvent;
using core::tui::runtime::ModalComponent;
using core::tui::runtime::runModal;
using core::tui::runtime::TuiRuntime;
using core::tui::runtime::TuiRuntimeOptions;
using core::tui::runtime::testing::ScriptedInputSource;

namespace
{

/// A trivial modal that completes with the codepoint of the first key it sees.
class KeyCaptureModal: public ModalComponent<char32_t>
{
  public:
    void render(Canvas& /*canvas*/) override {}

    [[nodiscard]] std::optional<char32_t> step(InputEvent const& event) override
    {
        if (auto const* key = std::get_if<KeyEvent>(&event))
            return key->codepoint;
        return std::nullopt;
    }
};

} // namespace

TEST_CASE("runModal resolves with the modal's result", "[Modal]")
{
    auto pipe = core::platform::createSystemPipe();
    REQUIRE(pipe.has_value());
    auto const backend = core::net::makeDefaultBackend();
    auto loop = EventLoop { *backend };
    auto source = ScriptedInputSource { pipe->get() };
    source.pushEvents({ InputEvent { KeyEvent { .codepoint = U'q' } } });
    auto runtime = TuiRuntime { loop, source };
    auto modal = KeyCaptureModal {};

    auto const result = runtime.blockOn(runModal(&runtime, &modal));

    REQUIRE(result.has_value());
    REQUIRE(*result == U'q');
}

TEST_CASE("runModal returns nullopt when cancelled", "[Modal]")
{
    auto pipe = core::platform::createSystemPipe();
    REQUIRE(pipe.has_value());
    auto const backend = core::net::makeDefaultBackend();
    auto loop = EventLoop { *backend };
    auto source = ScriptedInputSource { pipe->get() };
    auto wakeup = core::platform::Wakeup {};
    auto runtime = TuiRuntime { loop, source, TuiRuntimeOptions { .interruptWakeup = &wakeup } };
    auto modal = KeyCaptureModal {};

    core::platform::SignalHandler::simulateSigint();
    wakeup.signal();

    auto const result = runtime.blockOn(runModal(&runtime, &modal));

    REQUIRE_FALSE(result.has_value());
    REQUIRE_FALSE(core::platform::SignalHandler::hasPendingSigint());
}

TEST_CASE("runModal keeps stepping until the modal completes", "[Modal]")
{
    auto pipe = core::platform::createSystemPipe();
    REQUIRE(pipe.has_value());
    auto const backend = core::net::makeDefaultBackend();
    auto loop = EventLoop { *backend };
    auto source = ScriptedInputSource { pipe->get() };
    // First a mouse move (ignored by the modal), then the key that resolves it.
    source.pushEvents({ InputEvent { core::tui::MouseEvent { .type = core::tui::MouseEvent::Type::Move } } });
    source.pushEvents({ InputEvent { KeyEvent { .codepoint = U'k' } } });
    auto runtime = TuiRuntime { loop, source };
    auto modal = KeyCaptureModal {};

    auto const result = runtime.blockOn(runModal(&runtime, &modal));

    REQUIRE(result.has_value());
    REQUIRE(*result == U'k');
}
