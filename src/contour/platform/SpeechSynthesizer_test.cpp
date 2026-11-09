// SPDX-License-Identifier: Apache-2.0
//
// What gets spoken, and whether the feature offers itself at all. The synthesizer behind it is an
// optional Qt module and a platform voice, neither of which a test can rely on — so the part that is
// pinned here is the part that is ours: the text preparation, and the decision to stay quiet.

#include <contour/command/ContextMenu.hpp>
#include <contour/platform/SpeechSynthesizer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <QtTest/QTest>

using contour::platform::NullSpeechSynthesizer;
using contour::platform::speakableText;

TEST_CASE("speakableText drops what only exists for the eye", "[contour][speech]")
{
    auto constexpr Max = size_t { 4000 };

    SECTION("grid padding is silence")
    {
        // A terminal line exists out to the right margin whether or not anything was written there, so
        // a selection carries the blanks along. Spoken verbatim they are a long pause per line.
        CHECK(speakableText("hello          \nworld     ", Max) == "hello\nworld");
        CHECK(speakableText("tabbed\t\t\n", Max) == "tabbed");
        // CRLF from a pasted or captured buffer must not leave a stray carriage return behind.
        CHECK(speakableText("one   \r\ntwo\r\n", Max) == "one\ntwo");
    }

    SECTION("a run of blank lines becomes one pause")
    {
        CHECK(speakableText("a\n\n\n\n\nb", Max) == "a\n\nb");
        // ...and padding-only lines count as blank, which is what makes the collapse work on real
        // terminal output rather than only on hand-written strings.
        CHECK(speakableText("a\n   \n      \nb", Max) == "a\n\nb");
    }

    SECTION("leading and trailing blank lines are pure padding")
    {
        CHECK(speakableText("\n\n  \nhello\n\n\n", Max) == "hello");
    }

    SECTION("nothing worth saying comes back empty")
    {
        CHECK(speakableText("", Max).empty());
        CHECK(speakableText("\n\n\n", Max).empty());
        CHECK(speakableText("      \n   ", Max).empty());
    }
}

TEST_CASE("speakableText bounds how much it will read", "[contour][speech]")
{
    // Selecting a build log must not become minutes of speech with no way to skip ahead.
    auto text = std::string {};
    for (auto i = 0; i < 500; ++i)
        text += "line " + std::to_string(i) + "\n";

    auto const spoken = speakableText(text, 100);
    CHECK(spoken.size() <= 100);
    CHECK_FALSE(spoken.empty());
    // Cut at a line boundary, so speech ends on something that sounds finished rather than mid-word.
    CHECK(spoken.back() != ' ');
    CHECK(spoken.find("line 0") == 0);

    SECTION("a single enormous line is still bounded")
    {
        // No newline to cut at: the bound must still hold rather than deferring to a boundary that is
        // not there.
        CHECK(speakableText(std::string(5000, 'x'), 100).size() == 100);
    }
}

TEST_CASE("without a speech engine the feature does not offer itself", "[contour][speech]")
{
    auto const nullSpeech = NullSpeechSynthesizer {};
    CHECK_FALSE(nullSpeech.available());

    // ...and the menu row goes with it. A row that is permanently dead teaches the user the feature is
    // broken, when in truth this build or this machine simply has no voice.
    auto state = contour::command::ContextMenuState {};
    state.hasSelection = true;
    state.canSpeak = false;
    auto const withoutSpeech = contour::command::buildContextMenu(state);
    auto const hasReadAloud = [](auto const& entries) {
        return std::ranges::any_of(entries, [](auto const& e) { return e.title == "Read Aloud"; });
    };
    CHECK_FALSE(hasReadAloud(withoutSpeech));

    state.canSpeak = true;
    CHECK(hasReadAloud(contour::command::buildContextMenu(state)));

    // With a voice but nothing selected there is nothing to read, so the row stays away.
    state.hasSelection = false;
    CHECK_FALSE(hasReadAloud(contour::command::buildContextMenu(state)));
}

// {{{ ThreadedSpeechSynthesizer
//
// The real engine takes a second or more to come up (speech-dispatcher enumerates every installed
// voice), which must never happen on the GUI thread. These pin the threading contract with an engine the
// test controls.

namespace
{

using contour::platform::SpeechSynthesizer;
using contour::platform::ThreadedSpeechSynthesizer;

/// What a FakeSpeechEngine saw, kept outside it because the synthesizer owns (and destroys) the engine.
struct EngineLog
{
    std::mutex mutex;
    std::condition_variable changed;
    /// Every call, in order: "say:<text>", "stop", "available", "destroyed".
    std::vector<std::string> calls;
    /// The thread each entry of calls was made on.
    std::vector<std::thread::id> threads;
    /// The thread the factory ran on.
    std::optional<std::thread::id> factoryThread;

    void record(std::string call)
    {
        {
            auto const lock = std::scoped_lock { mutex };
            calls.push_back(std::move(call));
            threads.push_back(std::this_thread::get_id());
        }
        changed.notify_all();
    }

    /// Waits, bounded, until @p condition holds; it is evaluated under the lock, so it may read the members.
    /// @return Whether it did.
    template <typename Condition>
    [[nodiscard]] bool waitFor(Condition const& condition)
    {
        auto lock = std::unique_lock { mutex };
        return changed.wait_for(lock, std::chrono::seconds(10), condition);
    }

    [[nodiscard]] std::vector<std::string> snapshot()
    {
        auto const lock = std::scoped_lock { mutex };
        return calls;
    }
};

/// An engine that records each call and answers available() as told.
class FakeSpeechEngine final: public SpeechSynthesizer
{
  public:
    enum class Voice : uint8_t
    {
        None,
        Installed,
    };

    FakeSpeechEngine(std::shared_ptr<EngineLog> log, Voice voice): _log { std::move(log) }, _voice { voice }
    {
    }
    FakeSpeechEngine(FakeSpeechEngine const&) = delete;
    FakeSpeechEngine& operator=(FakeSpeechEngine const&) = delete;
    FakeSpeechEngine(FakeSpeechEngine&&) = delete;
    FakeSpeechEngine& operator=(FakeSpeechEngine&&) = delete;
    ~FakeSpeechEngine() override { _log->record("destroyed"); }

    [[nodiscard]] bool available() const override
    {
        _log->record("available");
        return _voice == Voice::Installed;
    }
    void say(std::string_view text) override { _log->record("say:" + std::string { text }); }
    void stop() override { _log->record("stop"); }

  private:
    std::shared_ptr<EngineLog> _log;
    Voice _voice;
};

/// A factory that records where it ran and builds a FakeSpeechEngine, after @p gate opens if given.
[[nodiscard]] ThreadedSpeechSynthesizer::EngineFactory fakeFactory(std::shared_ptr<EngineLog> log,
                                                                   FakeSpeechEngine::Voice voice,
                                                                   std::shared_future<void> gate = {})
{
    return [log = std::move(log), voice, gate = std::move(gate)]() -> std::unique_ptr<SpeechSynthesizer> {
        {
            auto const lock = std::scoped_lock { log->mutex };
            log->factoryThread = std::this_thread::get_id();
        }
        log->changed.notify_all();
        if (gate.valid())
            gate.wait();
        return std::make_unique<FakeSpeechEngine>(log, voice);
    };
}

/// Waits, bounded, until @p speech reports available.
[[nodiscard]] bool becomesAvailable(SpeechSynthesizer const& speech)
{
    return QTest::qWaitFor([&] { return speech.available(); }, 10'000);
}

} // namespace

TEST_CASE("ThreadedSpeechSynthesizer answers at once while the engine is still coming up",
          "[contour][speech][threading]")
{
    auto log = std::make_shared<EngineLog>();
    auto release = std::promise<void> {};
    auto speech = ThreadedSpeechSynthesizer { fakeFactory(
        log, FakeSpeechEngine::Voice::Installed, release.get_future().share()) };

    // The factory is parked on the gate, so an available() that waited for it would never return. It
    // returns, and says no: until the answer is in, the menus do not offer what may not work.
    CHECK_FALSE(speech.available());

    release.set_value();
    CHECK(becomesAvailable(speech));
}

TEST_CASE("ThreadedSpeechSynthesizer releases an engine that cannot speak", "[contour][speech][threading]")
{
    auto log = std::make_shared<EngineLog>();
    auto speech = ThreadedSpeechSynthesizer { fakeFactory(log, FakeSpeechEngine::Voice::None) };

    // The probe's answer is no, so the engine -- its plugin and service connection -- is let go at once...
    REQUIRE(log->waitFor([&] { return log->calls.size() >= 2; }));
    CHECK(log->snapshot() == std::vector<std::string> { "available", "destroyed" });

    // ...and what is asked of it afterwards finds no engine and does nothing.
    speech.say("into the void");
    speech.stop();
    CHECK_FALSE(speech.available());
}

TEST_CASE("ThreadedSpeechSynthesizer treats a factory without an engine as unavailable",
          "[contour][speech][threading]")
{
    auto speech = ThreadedSpeechSynthesizer { [] { return std::unique_ptr<SpeechSynthesizer> {}; } };

    speech.say("into the void");
    speech.stop();
    CHECK_FALSE(speech.available());
}

TEST_CASE("ThreadedSpeechSynthesizer speaks on its own thread, in order", "[contour][speech][threading]")
{
    auto log = std::make_shared<EngineLog>();
    auto release = std::promise<void> {};
    {
        auto speech = ThreadedSpeechSynthesizer { fakeFactory(
            log, FakeSpeechEngine::Voice::Installed, release.get_future().share()) };

        // Asked before the engine exists: queued behind its creation rather than dropped or run here.
        speech.stop();
        speech.say("first");
        speech.stop();
        speech.say("second");
        release.set_value();

        REQUIRE(log->waitFor([&] { return log->calls.size() >= 5; }));
    }

    auto const lock = std::scoped_lock { log->mutex };
    CHECK(
        log->calls
        == std::vector<std::string> { "available", "stop", "say:first", "stop", "say:second", "destroyed" });
    REQUIRE(log->factoryThread.has_value());
    CHECK(*log->factoryThread != std::this_thread::get_id());
    // Every call, the engine's destruction included, happened on the thread that built it.
    CHECK(std::ranges::all_of(log->threads, [&](auto id) { return id == *log->factoryThread; }));
}

TEST_CASE("ThreadedSpeechSynthesizer shuts down cleanly", "[contour][speech][threading]")
{
    auto log = std::make_shared<EngineLog>();

    SECTION("with an idle engine")
    {
        {
            auto speech = ThreadedSpeechSynthesizer { fakeFactory(log, FakeSpeechEngine::Voice::Installed) };
            REQUIRE(becomesAvailable(speech));
        }
        auto const calls = log->snapshot();
        CHECK(calls == std::vector<std::string> { "available", "destroyed" });
    }

    SECTION("while the engine is still coming up")
    {
        auto release = std::promise<void> {};
        auto releaser = std::jthread {};
        {
            auto speech = ThreadedSpeechSynthesizer { fakeFactory(
                log, FakeSpeechEngine::Voice::Installed, release.get_future().share()) };
            // The factory is running and parked; the gate opens from elsewhere, so the destructor below
            // finds it either still parked or just done. Both must end with the engine built and then
            // destroyed on its own thread.
            REQUIRE(log->waitFor([&] { return log->factoryThread.has_value(); }));
            releaser = std::jthread { [&release] { release.set_value(); } };
        }
        auto const lock = std::scoped_lock { log->mutex };
        REQUIRE(!log->calls.empty());
        CHECK(log->calls.back() == "destroyed");
        REQUIRE(log->factoryThread.has_value());
        CHECK(log->threads.back() == *log->factoryThread);
    }
}

// }}}
