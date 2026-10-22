// SPDX-License-Identifier: Apache-2.0
//
// The terminal's side of status-line extensions: the {Extension} segment, the take-over of the indicator
// row, and the event hooks -- all against a fake IStatusLineExtensions, since vtbackend runs no extension.
#include <vtbackend/screen/StatusLineBuilder.hpp>
#include <vtbackend/screen/StatusLineExtensions.hpp>
#include <vtbackend/screen/Terminal.hpp>
#include <vtbackend/testing/FakeStatusLineExtensions.hpp>
#include <vtbackend/testing/MockTerm.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

using namespace vtbackend;
using namespace std::string_literals;
using namespace std::string_view_literals;

namespace
{

constexpr auto ExtensionPageSize = PageSize { LineCount(5), ColumnCount(40) };

/// @return A snapshot whose extend-mode text is @p sgrText and whose one row is @p vt as a terminal draws it.
[[nodiscard]] std::shared_ptr<StatusLineExtensionSnapshot const> snapshotOf(std::string sgrText,
                                                                            std::string_view vt = {},
                                                                            uint64_t generation = 1)
{
    auto surface = MockTerm { PageSize { LineCount(1), ColumnCount(20) } };
    surface.terminal.setCellPixelSize(ImageSize { Width(8), Height(16) });
    surface.writeToScreen(vt);
    // Detached, as an extension's Surface detaches them: the surface terminal dies with this function.
    auto line = surface.terminal.primaryScreen().grid().lineAt(LineOffset(0));
    rehomeImageFragments(line, &detachedImage);
    return std::make_shared<StatusLineExtensionSnapshot const>(StatusLineExtensionSnapshot {
        .lines = { std::move(line) },
        .sgrText = std::move(sgrText),
        .generation = generation,
    });
}

/// @return A snapshot whose text is @p text, built without a terminal so a worker can publish many fast.
[[nodiscard]] std::shared_ptr<StatusLineExtensionSnapshot const> textSnapshot(std::string text,
                                                                              uint64_t generation)
{
    return std::make_shared<StatusLineExtensionSnapshot const>(StatusLineExtensionSnapshot {
        .lines = { Line(ColumnCount(20)) }, .sgrText = std::move(text), .generation = generation });
}

/// @return The indicator status line's row, trailing blanks trimmed.
[[nodiscard]] std::string indicatorText(Terminal const& terminal)
{
    return terminal.indicatorStatusLineDisplay().grid().lineAt(LineOffset(0)).toUtf8Trimmed();
}

/// Shows the indicator status line with @p left as its left segment, and repaints it.
void showIndicator(Terminal& terminal, std::string_view left)
{
    terminal.setStatusLineDefinition(parseStatusLineDefinition(left, "", ""));
    terminal.setStatusDisplay(StatusDisplayType::Indicator);
    terminal.refreshRenderBuffer();
}

/// One hook: what the terminal is made to do, and the event it must then hand the extensions.
struct HookCase
{
    std::string_view name;
    StatusLineEventKind kind;
    std::function<void(MockTerm<>&)> trigger;
    StatusLineEvent expected;
};

[[nodiscard]] std::vector<HookCase> hookCases()
{
    using namespace StatusLineEvents;
    return {
        { "title",
          StatusLineEventKind::Title,
          [](auto& mock) { mock.writeToScreen("\033]2;hello\033\\"); },
          TitleChanged { .title = "hello" } },
        { "cwd",
          StatusLineEventKind::Cwd,
          [](auto& mock) { mock.writeToScreen("\033]7;file:///tmp/x\033\\"); },
          CwdChanged { .cwd = "/tmp/x" } },
        { "foreground process",
          StatusLineEventKind::ForegroundProcess,
          [](auto& mock) { mock.terminal.foregroundProcessChanged("vim"); },
          ForegroundProcessChanged { .name = "vim" } },
        { "input mode",
          StatusLineEventKind::InputMode,
          [](auto& mock) { mock.terminal.inputHandler().setMode(ViMode::Normal); },
          InputModeChanged { .mode = ViMode::Normal } },
        { "tabs",
          StatusLineEventKind::Tabs,
          [](auto& mock) {
              mock.terminal.setGuiTabInfoForStatusLine(TabsInfo {
                  .tabs = { { .name = "one", .color = {} }, { .name = std::nullopt, .color = {} } },
                  .activeTabPosition = 2 });
          },
          TabsChanged { .tabs = { { .title = "one", .activity = StatusLineTabActivity::Inactive },
                                  { .title = "", .activity = StatusLineTabActivity::Active } } } },
        { "user var",
          StatusLineEventKind::UserVar,
          [](auto& mock) { mock.writeToScreen("\033]1337;SetUserVar=foo=YmFy\033\\"); },
          UserVarChanged { .name = "foo", .value = "bar" } },
        { "progress",
          StatusLineEventKind::Progress,
          [](auto& mock) { mock.writeToScreen("\033]9;4;1;42\033\\"); },
          ProgressChanged { .progress = Progress { .state = ProgressState::Normal, .percentage = 42 } } },
        { "focus",
          StatusLineEventKind::Focus,
          [](auto& mock) { (void) mock.terminal.sendFocusOutEvent(); },
          FocusChanged { .state = StatusLineFocus::Unfocused } },
    };
}

} // namespace

TEST_CASE("Terminal.extensions.segment", "[extensions]")
{
    auto extensions = FakeStatusLineExtensions {};
    extensions.publish("git", snapshotOf("main*"));
    auto mock = MockTerm { ExtensionPageSize, extensions.services() };

    showIndicator(mock.terminal, "{Extension:git}");
    CHECK(indicatorText(mock.terminal) == "main*");

    SECTION("styles and adornments wrap it as they wrap any item")
    {
        showIndicator(mock.terminal, "{Extension:Name=git,Left=[,Right=]}");
        CHECK(indicatorText(mock.terminal) == "[main*]");
    }

    SECTION("its SGR colours the text, and is not measured as columns")
    {
        extensions.publish("git", snapshotOf("\033[1mab\033[m"));
        mock.terminal.setStatusLineDefinition(parseStatusLineDefinition("", "", "{Extension:git}"));
        mock.terminal.refreshRenderBuffer();
        auto const& row = mock.terminal.indicatorStatusLineDisplay();
        auto const last = row.pageSize().columns.as<int>() - 1;
        CHECK(row.grid().lineAt(LineOffset(0)).toUtf8Trimmed().ends_with("ab"));
        CHECK(row.at(LineOffset(0), ColumnOffset(last)).toUtf8() == "b");
        CHECK(row.at(LineOffset(0), ColumnOffset(last)).isFlagEnabled(CellFlag::Bold));
    }

    SECTION("an extension that has not rendered yet shows nothing")
    {
        showIndicator(mock.terminal, "<{Extension:other}>");
        CHECK(indicatorText(mock.terminal) == "<>");
    }
}

TEST_CASE("Terminal.extensions.takeOver", "[extensions]")
{
    auto extensions = FakeStatusLineExtensions {};
    extensions.takeOver("plasma");
    auto mock = MockTerm { ExtensionPageSize, extensions.services() };
    auto& terminal = mock.terminal;

    SECTION("before its first frame the row is empty, and the template does not run")
    {
        showIndicator(terminal, "template");
        CHECK(indicatorText(terminal).empty());
    }

    SECTION("its frame is the row")
    {
        extensions.publish("plasma", snapshotOf("unused", "\033[1mX\033[m."));
        showIndicator(terminal, "template");
        auto const& row = terminal.indicatorStatusLineDisplay();
        CHECK(indicatorText(terminal) == "X.");
        CHECK(row.at(LineOffset(0), ColumnOffset(0)).isFlagEnabled(CellFlag::Bold));
        CHECK_FALSE(row.at(LineOffset(0), ColumnOffset(1)).isFlagEnabled(CellFlag::Bold));
        // Fitted to the status line, however wide the extension's surface was.
        CHECK(row.grid().lineAt(LineOffset(0)).size() == row.pageSize().columns);
        CHECK(extensions.snapshotCalls("plasma") > 0);

        // DECSSDT still switches to the host-writable status line and back.
        mock.writeToScreen("\033[2$~");
        terminal.answerHostWritableStatusLineRequest(
            HostWritableStatusLineAnswer::Shown); // as a frontend does
        CHECK(terminal.statusDisplayType() == StatusDisplayType::HostWritable);
        mock.writeToScreen("\033[1$~");
        CHECK(terminal.statusDisplayType() == StatusDisplayType::Indicator);
        terminal.refreshRenderBuffer();
        CHECK(indicatorText(terminal) == "X.");
    }
}

namespace
{
/// Two cells of red: 16 by 6 pixels, at 8 by 16 pixels a cell.
constexpr auto RedSixel = "\033Pq#0;2;100;0;0#0!16~\033\\"sv;

/// @return The colours the indicator row of @p terminal wears in insert mode.
[[nodiscard]] RGBColorPair indicatorColors(Terminal const& terminal)
{
    return terminal.focused() ? terminal.colorPalette().indicatorStatusLineInsertMode
                              : terminal.colorPalette().indicatorStatusLineInactive;
}

[[nodiscard]] ImageId imageIdAt(Screen const& screen, ColumnOffset column)
{
    auto const fragment = screen.at(LineOffset(0), column).imageFragment();
    REQUIRE(fragment != nullptr);
    return fragment->rasterizedImage().image().id();
}
} // namespace

TEST_CASE("Terminal.extensions.takeOver.rehomes", "[extensions]")
{
    // The frame came from another terminal's grid: its images get this terminal's ids and remover, and
    // its hyperlink ids, which name another terminal's store, are dropped.
    auto extensions = FakeStatusLineExtensions {};
    extensions.takeOver("plasma");
    auto mock = MockTerm { ExtensionPageSize, extensions.services() };
    auto& terminal = mock.terminal;
    terminal.setCellPixelSize(ImageSize { Width(8), Height(16) });
    mock.writeToScreen(RedSixel); // the host's own first image: the id the frame's image had too
    auto const mainPageImage = imageIdAt(terminal.primaryScreen(), ColumnOffset(0));

    extensions.publish(
        "plasma", snapshotOf("", std::format("{}\033[1;3H\033]8;;https://x\033\\L\033]8;;\033\\", RedSixel)));
    auto const& frameFragments = extensions.snapshot("plasma")->lines[0].storage().imageFragments;
    REQUIRE(frameFragments.has_value());
    REQUIRE(frameFragments->at(0)->rasterizedImage().image().id() == mainPageImage); // the collision

    showIndicator(terminal, "template");
    auto const& row = terminal.indicatorStatusLineDisplay();
    auto const rowImage = imageIdAt(row, ColumnOffset(0));
    CHECK(rowImage != mainPageImage);
    CHECK(imageIdAt(row, ColumnOffset(1)) == rowImage); // one image across its cells
    CHECK(terminal.imagePool().findImageById(rowImage) != nullptr);
    CHECK(row.at(LineOffset(0), ColumnOffset(2)).toUtf8() == "L");
    CHECK(row.at(LineOffset(0), ColumnOffset(2)).hyperlink() == HyperlinkId {});

    SECTION("an unchanged frame is not re-created on a repaint")
    {
        terminal.refreshRenderBuffer();
        CHECK(imageIdAt(terminal.indicatorStatusLineDisplay(), ColumnOffset(0)) == rowImage);
    }

    SECTION("a new frame is")
    {
        extensions.publish("plasma", snapshotOf("", RedSixel, 2));
        terminal.refreshRenderBuffer();
        CHECK(imageIdAt(terminal.indicatorStatusLineDisplay(), ColumnOffset(0)) != rowImage);
    }

    SECTION("the row's images are the host's, whatever becomes of the frame")
    {
        extensions.publish("plasma", nullptr); // the extension let go of its frame
        CHECK(terminal.imagePool().findImageById(rowImage) != nullptr);
        CHECK(!terminal.imagePool().findImageById(rowImage)->data().empty());
        terminal.refreshRenderBuffer(); // and once the row lets go of it too, it is gone
        CHECK(indicatorText(terminal).empty());
    }

    // Read once: it is fixed for the extensions' life.
    CHECK(extensions.takeOverNameCalls() == 1);
}

TEST_CASE("Terminal.extensions.defaultColors", "[extensions]")
{
    // Cells left in the default colours show in the status line's, not the main page's.
    auto extensions = FakeStatusLineExtensions {};
    auto const expectColors = [](Terminal const& terminal, ColumnOffset column) {
        auto const cell = terminal.indicatorStatusLineDisplay().at(LineOffset(0), column);
        auto const colors = indicatorColors(terminal);
        CHECK(cell.foregroundColor() == Color(colors.foreground));
        CHECK(cell.backgroundColor() == Color(colors.background));
    };

    SECTION("take-over")
    {
        extensions.takeOver("plasma");
        extensions.publish("plasma", snapshotOf("", "\033[1mX\033[m."));
        auto mock = MockTerm { ExtensionPageSize, extensions.services() };
        showIndicator(mock.terminal, "");
        REQUIRE(indicatorText(mock.terminal) == "X.");
        expectColors(mock.terminal, ColumnOffset(0));
        expectColors(mock.terminal, ColumnOffset(1));
        expectColors(mock.terminal, ColumnOffset(10)); // past the frame's text
    }

    SECTION("a reset inside a segment's text")
    {
        extensions.publish("git", snapshotOf("\033[1ma\033[mb"));
        auto mock = MockTerm { ExtensionPageSize, extensions.services() };
        showIndicator(mock.terminal, "{Extension:git}c");
        REQUIRE(indicatorText(mock.terminal) == "abc");
        expectColors(mock.terminal, ColumnOffset(1));
        expectColors(mock.terminal, ColumnOffset(2));
    }
}

TEST_CASE("StatusLineEventGate.buildsOnlyWhatIsPosted", "[extensions]")
{
    // What a hook builds, decodes or allocates is built in the lambda, which runs only when subscribed:
    // OSC 1337 SetUserVar's base64 decode included.
    auto extensions = FakeStatusLineExtensions {};
    auto builds = 0;
    auto const make = [&] {
        ++builds;
        return StatusLineEvents::UserVarChanged { .name = "n", .value = "v" };
    };

    auto unsubscribed = StatusLineEventGate { &extensions };
    unsubscribed.post(make);
    CHECK(builds == 0);
    CHECK(extensions.posted().empty());

    extensions.subscribe(StatusLineEventKinds { StatusLineEventKind::UserVar });
    unsubscribed.post(make); // still the cached subscriptions
    CHECK(builds == 0);
    unsubscribed.refresh();
    unsubscribed.post(make);
    CHECK(builds == 1);
    CHECK(extensions.posted().size() == 1);

    auto none = StatusLineEventGate { nullptr };
    none.refresh();
    none.post(make);
    CHECK(builds == 1);
}

TEST_CASE("StatusLineSubscriptionWatch", "[extensions]")
{
    // Only a change of whether the watched kind is followed counts, from nothing followed at first.
    auto watch = StatusLineSubscriptionWatch { StatusLineEventKind::ForegroundProcess };
    auto const following = StatusLineEventKinds { StatusLineEventKind::ForegroundProcess };
    auto const other = StatusLineEventKinds { StatusLineEventKind::Cwd };
    CHECK_FALSE(watch.changed(StatusLineEventKinds {}));
    CHECK_FALSE(watch.changed(other));
    CHECK(watch.changed(following));
    CHECK_FALSE(watch.changed(following | other));
    CHECK(watch.changed(other));
    CHECK_FALSE(watch.changed(StatusLineEventKinds {}));

    // Reset, what was followed is forgotten: the next following set is a change again.
    CHECK(watch.changed(following));
    watch.reset();
    CHECK(watch.changed(following));
}

TEST_CASE("StatusLinePublishSink", "[extensions]")
{
    struct CountingTarget final: IStatusLinePublishTarget
    {
        std::atomic<int> delivered = 0;
        void statusLineExtensionsPublished() noexcept override { ++delivered; }
    };

    auto sink = StatusLinePublishSink {};
    auto target = CountingTarget {};
    sink.publish(); // nobody attached yet
    sink.attach(target);
    sink.publish();
    CHECK(target.delivered == 1);
    sink.revoke();
    sink.publish();
    CHECK(target.delivered == 1);
}

TEST_CASE("StatusLinePublishSink.revokeWaitsForADeliveryInFlight", "[extensions][threading]")
{
    struct BlockingTarget final: IStatusLinePublishTarget
    {
        std::atomic<bool> entered = false;
        std::atomic<bool> release = false;
        void statusLineExtensionsPublished() noexcept override
        {
            entered = true;
            while (!release.load())
                std::this_thread::yield();
        }
    };

    auto sink = StatusLinePublishSink {};
    auto target = BlockingTarget {};
    sink.attach(target);
    auto publisher = std::thread { [&] { sink.publish(); } };
    while (!target.entered.load())
        std::this_thread::yield();

    auto revoked = std::atomic<bool> { false };
    auto revoker = std::thread { [&] {
        sink.revoke();
        revoked = true;
    } };
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK_FALSE(revoked.load()); // still delivering
    target.release = true;
    publisher.join();
    revoker.join();
    CHECK(revoked.load());
}

TEST_CASE("Terminal.extensions.hooks", "[extensions]")
{
    auto extensions = FakeStatusLineExtensions {};
    extensions.subscribe(StatusLineEventKinds { StatusLineEventKind::Cwd });
    auto mock = MockTerm { ExtensionPageSize, extensions.services() };

    mock.writeToScreen("\033]7;file:///tmp/x\033\\");
    auto posted = extensions.posted();
    REQUIRE(posted.size() == 1);
    CHECK(posted[0] == StatusLineEvent { StatusLineEvents::CwdChanged { .cwd = "/tmp/x" } });

    mock.writeToScreen("\033]0;title\033\\");
    CHECK(extensions.posted().size() == 1);

    SECTION("every hook posts its own kind, and only when subscribed to")
    {
        for (auto const& hook: hookCases())
        {
            INFO(hook.name);
            auto subscribed = FakeStatusLineExtensions {};
            subscribed.subscribe(StatusLineEventKinds { hook.kind });
            auto terminal = MockTerm { ExtensionPageSize, subscribed.services() };
            hook.trigger(terminal);
            posted = subscribed.posted();
            REQUIRE(posted.size() == 1);
            CHECK(kindOf(posted[0]) == hook.kind);
            CHECK(posted[0] == hook.expected);

            // Every other hook is subscribed to by nobody here, so it posts nothing.
            for (auto const& other: hookCases())
                if (other.kind != hook.kind)
                    other.trigger(terminal);
            CHECK(subscribed.posted().size() == 1);
        }
    }

    SECTION("the same tabs again post nothing")
    {
        auto subscribed = FakeStatusLineExtensions {};
        subscribed.subscribe(StatusLineEventKinds { StatusLineEventKind::Tabs });
        auto terminal = MockTerm { ExtensionPageSize, subscribed.services() };
        auto const tabs = TabsInfo { .tabs = { { .name = "one", .color = {} } }, .activeTabPosition = 1 };
        terminal.terminal.setGuiTabInfoForStatusLine(TabsInfo { tabs });
        terminal.terminal.setGuiTabInfoForStatusLine(TabsInfo { tabs });
        CHECK(subscribed.posted().size() == 1);
    }

    SECTION("the status line's size is told on construction and when it changes")
    {
        auto const resizes = extensions.resizes();
        REQUIRE(resizes.size() == 1);
        CHECK(resizes[0] == PageSize { LineCount(1), ColumnCount(40) });

        mock.terminal.resizeScreen(PageSize { LineCount(5), ColumnCount(50) });
        REQUIRE(extensions.resizes().size() == 2);
        CHECK(extensions.resizes()[1] == PageSize { LineCount(1), ColumnCount(50) });

        // A repaint at the same size tells nothing new.
        showIndicator(mock.terminal, "{Extension:git}");
        CHECK(extensions.resizes().size() == 2);
    }
}

TEST_CASE("Terminal.extensions.published", "[extensions]")
{
    // The subscriptions are cached, and refreshed when a frame is published: the hooks never ask the
    // extensions, which would be a virtual call (and the host's walk over its runners) per event.
    auto extensions = FakeStatusLineExtensions {};
    auto mock = MockTerm { ExtensionPageSize, extensions.services() };
    auto& terminal = mock.terminal;

    extensions.subscribe(StatusLineEventKinds { StatusLineEventKind::Cwd });
    mock.writeToScreen("\033]7;file:///a\033\\");
    CHECK(extensions.posted().empty());

    terminal.ensureFreshRenderBuffer(); // settle the buffer so the repaint request is observable
    REQUIRE(terminal.renderBufferState() == RenderBufferState::WaitingForRefresh);
    terminal.statusLineExtensionsPublished();
    CHECK(terminal.renderBufferState() == RenderBufferState::RefreshBuffersAndTrySwap);

    mock.writeToScreen("\033]7;file:///b\033\\");
    CHECK(extensions.posted().size() == 1);
}

TEST_CASE("Terminal.extensions.replaced", "[extensions]")
{
    // The extensions were swapped underneath the terminal (a configuration reload): it reads the take-over
    // name anew, tells the new ones the status line's size, and re-reads what they subscribe to.
    auto extensions = FakeStatusLineExtensions {};
    extensions.takeOver("a");
    extensions.publish("a", snapshotOf("unused", "AAA"));
    auto mock = MockTerm { ExtensionPageSize, extensions.services() };
    auto& terminal = mock.terminal;
    showIndicator(terminal, "template");
    REQUIRE(indicatorText(terminal) == "AAA");
    auto const resizesBefore = extensions.resizes().size();

    extensions.takeOver("b");
    extensions.publish("b", snapshotOf("unused", "BBB"));
    extensions.subscribe(StatusLineEventKinds { StatusLineEventKind::Cwd });
    terminal.runUnderStateLock([&] { terminal.statusLineExtensionsReplaced(); });

    // The new extensions were told the size they draw at, however unchanged it is.
    REQUIRE(extensions.resizes().size() == resizesBefore + 1);
    CHECK(extensions.resizes().back() == PageSize { LineCount(1), ExtensionPageSize.columns });
    terminal.refreshRenderBuffer();
    CHECK(indicatorText(terminal) == "BBB");
    mock.writeToScreen("\033]7;file:///b\033\\");
    CHECK(extensions.posted().size() == 1);

    SECTION("and back to the template when none takes over any more")
    {
        extensions.takeOver(std::nullopt);
        terminal.runUnderStateLock([&] { terminal.statusLineExtensionsReplaced(); });
        terminal.refreshRenderBuffer();
        CHECK(indicatorText(terminal) == "template");
    }
}

TEST_CASE("Terminal.extensions.font", "[extensions]")
{
    // The font is told to every extension, subscribed or not -- each one's surface takes its cell size from
    // it -- once per change, and again to extensions loaded or subscribing later.
    using namespace StatusLineEvents;
    auto const small =
        StatusLineFontMetrics { .pointSize = 12, .cellPixelSize = ImageSize { Width(9), Height(18) } };
    auto const large =
        StatusLineFontMetrics { .pointSize = 16, .cellPixelSize = ImageSize { Width(12), Height(24) } };
    auto extensions = FakeStatusLineExtensions {};
    auto mock = MockTerm { ExtensionPageSize, extensions.services() };
    auto& terminal = mock.terminal;
    auto const setFont = [&](StatusLineFontMetrics metrics) {
        terminal.runUnderStateLock([&] { terminal.setStatusLineFontMetrics(metrics); });
    };
    auto const fontEvents = [&] {
        return std::ranges::count_if(extensions.posted(), [](StatusLineEvent const& event) {
            return std::holds_alternative<FontChanged>(event);
        });
    };

    SECTION("nothing is told before the host knows the font")
    {
        extensions.subscribe(StatusLineEventKinds { StatusLineEventKind::Font });
        terminal.runUnderStateLock([&] { terminal.statusLineExtensionsReplaced(); });
        CHECK(fontEvents() == 0);
    }

    SECTION("told to every extension, once per change")
    {
        setFont(small);
        REQUIRE(extensions.posted().size() == 1);
        CHECK(extensions.posted().back() == StatusLineEvent { FontChanged { .metrics = small } });

        setFont(small);
        CHECK(extensions.posted().size() == 1);

        setFont(large);
        REQUIRE(extensions.posted().size() == 2);
        CHECK(extensions.posted().back() == StatusLineEvent { FontChanged { .metrics = large } });
    }

    SECTION("metrics known before a subscription are replayed to it")
    {
        setFont(small);
        extensions.subscribe(StatusLineEventKinds { StatusLineEventKind::Font });
        terminal.statusLineExtensionsPublished();
        terminal.refreshRenderBuffer();
        REQUIRE(fontEvents() == 2);
        CHECK(extensions.posted().back() == StatusLineEvent { FontChanged { .metrics = small } });
    }

    SECTION("replaced extensions are told the metrics once, subscribed or not")
    {
        setFont(small);
        extensions.subscribe(StatusLineEventKinds { StatusLineEventKind::Font });
        terminal.runUnderStateLock([&] { terminal.statusLineExtensionsReplaced(); });
        REQUIRE(fontEvents() == 2);
        CHECK(extensions.posted().back() == StatusLineEvent { FontChanged { .metrics = small } });
    }
}

TEST_CASE("Terminal.extensions.replay", "[extensions]")
{
    // An extension subscribes only once it has loaded, after the state it follows was set: what it newly
    // subscribes to is replayed to it once, from the locked frame refresh, never from the worker that
    // published.
    using namespace StatusLineEvents;
    auto extensions = FakeStatusLineExtensions { /*maxUserVars=*/2 };
    auto mock = MockTerm { ExtensionPageSize, extensions.services() };
    auto& terminal = mock.terminal;
    auto const tabs = TabsInfo { .tabs = { { .name = "one", .color = {} } }, .activeTabPosition = 1 };
    mock.writeToScreen("\033]7;file:///tmp/x\033\\\033]2;hello\033\\");
    terminal.setGuiTabInfoForStatusLine(TabsInfo { tabs });
    REQUIRE(extensions.posted().empty());

    extensions.subscribe(StatusLineEventKinds { StatusLineEventKind::Cwd, StatusLineEventKind::Tabs });
    terminal.statusLineExtensionsPublished();
    CHECK(extensions.posted().empty()); // the worker reads no terminal state
    terminal.refreshRenderBuffer();
    auto const posted = extensions.posted();
    REQUIRE(posted.size() == 2);
    CHECK(std::ranges::count(posted, StatusLineEvent { CwdChanged { .cwd = "/tmp/x" } }) == 1);
    CHECK(
        std::ranges::count(posted,
                           StatusLineEvent { TabsChanged {
                               .tabs = { { .title = "one", .activity = StatusLineTabActivity::Active } } } })
        == 1);

    // Once only: neither another frame nor the same tabs again (which the terminal dedupes) repeats it.
    terminal.refreshRenderBuffer();
    terminal.setGuiTabInfoForStatusLine(TabsInfo { tabs });
    CHECK(extensions.posted().size() == 2);

    SECTION("a kind subscribed later is replayed then, and only it")
    {
        extensions.subscribe(StatusLineEventKinds {
            StatusLineEventKind::Cwd, StatusLineEventKind::Tabs, StatusLineEventKind::Title });
        terminal.statusLineExtensionsPublished();
        terminal.refreshRenderBuffer();
        REQUIRE(extensions.posted().size() == 3);
        CHECK(extensions.posted().back() == StatusLineEvent { TitleChanged { .title = "hello" } });
    }

    SECTION("the progress indicator an application set is replayed too, and none while there is none")
    {
        extensions.subscribe(StatusLineEventKinds {
            StatusLineEventKind::Cwd, StatusLineEventKind::Tabs, StatusLineEventKind::Progress });
        terminal.statusLineExtensionsPublished();
        terminal.refreshRenderBuffer();
        CHECK(extensions.posted().size() == 2); // no indicator: nothing to tell

        mock.writeToScreen("\033]9;4;1;42\033\\");
        REQUIRE(extensions.posted().size() == 3); // live, now that it is subscribed
        extensions.subscribe(StatusLineEventKinds { StatusLineEventKind::Cwd, StatusLineEventKind::Tabs });
        terminal.statusLineExtensionsPublished();
        extensions.subscribe(StatusLineEventKinds {
            StatusLineEventKind::Cwd, StatusLineEventKind::Tabs, StatusLineEventKind::Progress });
        terminal.statusLineExtensionsPublished();
        terminal.refreshRenderBuffer();
        REQUIRE(extensions.posted().size() == 4);
        CHECK(extensions.posted().back()
              == StatusLineEvent { ProgressChanged {
                  .progress = Progress { .state = ProgressState::Normal, .percentage = 42 } } });
    }

    SECTION("replaced extensions are told everything they subscribe to")
    {
        terminal.runUnderStateLock([&] { terminal.statusLineExtensionsReplaced(); });
        CHECK(extensions.posted().size() == 4);
    }

    SECTION("the foreground process the host last told of is replayed")
    {
        // As a poller does, which tells a name only once: an extension loaded after it never hears it live.
        terminal.runUnderStateLock([&] { terminal.foregroundProcessChanged("vim"); });
        REQUIRE(extensions.posted().size() == 2);
        extensions.subscribe(StatusLineEventKinds {
            StatusLineEventKind::Cwd, StatusLineEventKind::Tabs, StatusLineEventKind::ForegroundProcess });
        terminal.statusLineExtensionsPublished();
        terminal.refreshRenderBuffer();
        REQUIRE(extensions.posted().size() == 3);
        CHECK(extensions.posted().back() == StatusLineEvent { ForegroundProcessChanged { .name = "vim" } });

        terminal.runUnderStateLock([&] { terminal.statusLineExtensionsReplaced(); });
        CHECK(std::ranges::count(extensions.posted(),
                                 StatusLineEvent { ForegroundProcessChanged { .name = "vim" } })
              == 2);
    }

    SECTION("the user variables set before are replayed, each with its latest value, up to the bound")
    {
        // The bound is the extension services' (2 here): a third name is not remembered.
        mock.writeToScreen("\033]1337;SetUserVar=a=b25l\033\\"     // a = one
                           "\033]1337;SetUserVar=b=dHdv\033\\"     // b = two
                           "\033]1337;SetUserVar=a=dGhyZWU=\033\\" // a = three
                           "\033]1337;SetUserVar=c=eA==\033\\");   // c = x, past the bound
        REQUIRE(extensions.posted().size() == 2);
        extensions.subscribe(StatusLineEventKinds {
            StatusLineEventKind::Cwd, StatusLineEventKind::Tabs, StatusLineEventKind::UserVar });
        terminal.statusLineExtensionsPublished();
        terminal.refreshRenderBuffer();
        auto const replayed = std::vector<StatusLineEvent> { UserVarChanged { .name = "a", .value = "three" },
                                                             UserVarChanged { .name = "b", .value = "two" } };
        auto const posted = extensions.posted();
        REQUIRE(posted.size() == 4);
        CHECK(std::vector<StatusLineEvent>(posted.begin() + 2, posted.end()) == replayed);

        // Replaced extensions are told them again.
        terminal.runUnderStateLock([&] { terminal.statusLineExtensionsReplaced(); });
        auto const again = extensions.posted();
        REQUIRE(again.size() == 8); // cwd, tabs, a, b
        CHECK(std::ranges::count(again, replayed[0]) == 2);
        CHECK(std::ranges::count(again, replayed[1]) == 2);
        CHECK(std::ranges::none_of(again, [](StatusLineEvent const& event) {
            auto const* const userVar = std::get_if<UserVarChanged>(&event);
            return userVar && userVar->name == "c";
        }));
    }
}

namespace
{
/// A MockTerm that counts the frames its terminal was told were published.
class PublishCountingTerm final: public MockTerm<>
{
  public:
    using MockTerm::MockTerm;

    void extensionFramePublished() override { ++published; }

    std::atomic<int> published = 0;
};
} // namespace

TEST_CASE("Terminal.extensions.publishedIsTold", "[extensions]")
{
    // A host that renders nothing itself -- the daemon -- learns of a frame from its events, on the worker
    // that published it, so that it can repaint the status line on its own thread.
    auto extensions = FakeStatusLineExtensions {};
    auto mock = PublishCountingTerm { ExtensionPageSize, extensions.services() };
    CHECK(mock.published == 0);
    mock.terminal.statusLineExtensionsPublished();
    CHECK(mock.published == 1);
}

TEST_CASE("Terminal.extensions.drivenByExtensions", "[extensions]")
{
    // Whether the indicator row shows anything only this terminal can draw: its extensions' frames.
    SECTION("without extensions it never is")
    {
        auto mock = MockTerm { ExtensionPageSize };
        showIndicator(mock.terminal, "{Extension:git}");
        CHECK_FALSE(mock.terminal.indicatorStatusLineDrivenByExtensions());
    }

    auto extensions = FakeStatusLineExtensions {};

    SECTION("a template naming no extension is not")
    {
        auto mock = MockTerm { ExtensionPageSize, extensions.services() };
        showIndicator(mock.terminal, "{Title} {Clock}");
        CHECK_FALSE(mock.terminal.indicatorStatusLineDrivenByExtensions());
    }

    SECTION("an {Extension} segment in any part of the template is")
    {
        auto mock = MockTerm { ExtensionPageSize, extensions.services() };
        mock.terminal.setStatusLineDefinition(parseStatusLineDefinition("", "", "[{Extension:git}]"));
        CHECK(mock.terminal.indicatorStatusLineDrivenByExtensions());
    }

    SECTION("a take-over is")
    {
        extensions.takeOver("plasma");
        auto mock = MockTerm { ExtensionPageSize, extensions.services() };
        showIndicator(mock.terminal, "template");
        CHECK(mock.terminal.indicatorStatusLineDrivenByExtensions());
    }
}

TEST_CASE("Terminal.extensions.refreshedWithoutRendering", "[extensions]")
{
    // What a frame does for the extensions, without a frame: the replay of what they newly follow, and the
    // indicator row repainted, so that a host that renders nothing can read the row and send it on.
    using namespace StatusLineEvents;
    auto extensions = FakeStatusLineExtensions {};
    extensions.publish("git", snapshotOf("main*"));
    auto mock = MockTerm { ExtensionPageSize, extensions.services() };
    auto& terminal = mock.terminal;
    terminal.setStatusLineDefinition(parseStatusLineDefinition("<{Extension:git}>", "", ""));
    terminal.setStatusDisplay(StatusDisplayType::Indicator);
    mock.writeToScreen("\033]7;file:///tmp/x\033\\");

    extensions.publish("git", snapshotOf("dev"));
    extensions.subscribe(StatusLineEventKinds { StatusLineEventKind::Cwd });
    terminal.statusLineExtensionsPublished();
    REQUIRE(extensions.posted().empty());

    terminal.runUnderStateLock([&] { terminal.refreshIndicatorStatusLine(); });
    CHECK(indicatorText(terminal) == "<dev>");
    CHECK(extensions.posted() == std::vector<StatusLineEvent> { CwdChanged { .cwd = "/tmp/x" } });

    SECTION("a take-over's frame is the row")
    {
        extensions.takeOver("git");
        extensions.publish("git", snapshotOf("unused", "dev"));
        terminal.runUnderStateLock([&] {
            terminal.statusLineExtensionsReplaced();
            terminal.refreshIndicatorStatusLine();
        });
        CHECK(indicatorText(terminal) == "dev");
    }

    SECTION("the row is repainted while this terminal shows no status line: its host's clients may")
    {
        terminal.setStatusDisplay(StatusDisplayType::None);
        extensions.publish("git", snapshotOf("away"));
        terminal.runUnderStateLock([&] { terminal.refreshIndicatorStatusLine(); });
        CHECK(indicatorText(terminal) == "<away>");
    }

    SECTION("nothing is repainted while no extension draws the row")
    {
        auto plain = MockTerm { ExtensionPageSize };
        plain.terminal.setStatusLineDefinition(parseStatusLineDefinition("<{Extension:git}>", "", ""));
        auto const before = indicatorText(plain.terminal);
        plain.writeToScreen("\033]2;title\033\\");
        plain.terminal.runUnderStateLock([&] { plain.terminal.refreshIndicatorStatusLine(); });
        CHECK(indicatorText(plain.terminal) == before);
        CHECK_FALSE(plain.terminal.indicatorStatusLineDrivenByExtensions());
    }
}

TEST_CASE("Terminal.mirroredIndicatorStatusLine", "[extensions]")
{
    // A mirror shows the row its host drew in place of its own template, and its own again once told none.
    auto mock = MockTerm { ExtensionPageSize };
    auto& terminal = mock.terminal;
    showIndicator(terminal, "local");
    REQUIRE(indicatorText(terminal) == "local");

    auto row = Line(ColumnCount(5));
    auto host = MockTerm { PageSize { LineCount(1), ColumnCount(10) } };
    host.writeToScreen("\033[1mHOST\033[m");
    row = host.terminal.primaryScreen().grid().lineAt(LineOffset(0));

    terminal.runUnderStateLock([&] { terminal.setMirroredIndicatorStatusLine(row); });
    terminal.refreshRenderBuffer();
    CHECK(indicatorText(terminal) == "HOST");
    CHECK(terminal.indicatorStatusLineDisplay()
              .at(LineOffset(0), ColumnOffset(0))
              .isFlagEnabled(CellFlag::Bold));
    // Fitted to the status line, however wide the host's was.
    CHECK(terminal.indicatorStatusLineDisplay().grid().lineAt(LineOffset(0)).size()
          == terminal.indicatorStatusLineDisplay().pageSize().columns);

    terminal.runUnderStateLock([&] { terminal.setMirroredIndicatorStatusLine(std::nullopt); });
    terminal.refreshRenderBuffer();
    CHECK(indicatorText(terminal) == "local");
}

TEST_CASE("Terminal.extensions.none", "[extensions]")
{
    SECTION("without services, an {Extension} renders empty and every hook is a no-op")
    {
        auto mock = MockTerm { ExtensionPageSize };
        showIndicator(mock.terminal, "<{Extension:git}>");
        CHECK(indicatorText(mock.terminal) == "<>");
        CHECK(serializeToVT(mock.terminal,
                            StatusLineSegment { StatusLineDefinitions::Extension { {}, "git" } },
                            StatusLineStyling::Enabled)
                  .empty());
        for (auto const& hook: hookCases())
            hook.trigger(mock);
        mock.terminal.statusLineExtensionsPublished();
    }

    SECTION("with services but no subscription, no hook posts anything")
    {
        auto extensions = FakeStatusLineExtensions {};
        auto mock = MockTerm { ExtensionPageSize, extensions.services() };
        for (auto const& hook: hookCases())
            hook.trigger(mock);
        CHECK(extensions.posted().empty());
    }
}

TEST_CASE("Terminal.extensions.sameNameTwice", "[extensions]")
{
    // One frame per repaint and name: both slots, and the measuring of them, read one snapshot, so a frame
    // published halfway through cannot show two generations side by side.
    auto extensions = FakeStatusLineExtensions {};
    extensions.publish("git", snapshotOf("main"));
    auto mock = MockTerm { ExtensionPageSize, extensions.services() };
    mock.terminal.setStatusLineDefinition(
        parseStatusLineDefinition("{Extension:git} {Extension:git}", "", ""));
    mock.terminal.setStatusDisplay(StatusDisplayType::Indicator);

    extensions.forgetSnapshotCalls();
    mock.terminal.refreshRenderBuffer();
    CHECK(indicatorText(mock.terminal) == "main main");
    CHECK(extensions.snapshotCalls("git") == 1);
}

TEST_CASE("Terminal.extensions.reloadInFlight", "[extensions][threading]")
{
    // A worker publishes frames, and tells the terminal so, while the definition is replaced under the
    // terminal's lock: no crash, no race, the row shows a frame of the name the definition now names, and
    // never one older than what was published when it was named.
    auto extensions = FakeStatusLineExtensions {};
    extensions.publish("a", textSnapshot("a0", 0));
    extensions.publish("b", textSnapshot("b0", 0));
    auto mock = MockTerm { ExtensionPageSize, extensions.services() };
    auto& terminal = mock.terminal;
    terminal.setStatusDisplay(StatusDisplayType::Indicator);

    auto stop = std::atomic<bool> { false };
    auto publisher = std::thread { [&] {
        auto generation = uint64_t { 0 };
        while (!stop.load())
        {
            ++generation;
            extensions.publish("a", textSnapshot(std::format("a{}", generation), generation));
            extensions.publish("b", textSnapshot(std::format("b{}", generation), generation));
            extensions.sink.publish();
        }
    } };

    for (auto const i: std::views::iota(0, 200))
    {
        auto const name = i % 2 == 0 ? "a"s : "b"s;
        auto text = std::string {};
        auto publishedBefore = uint64_t { 0 };
        {
            auto const lock = std::scoped_lock { terminal };
            publishedBefore = extensions.generationOf(name);
            terminal.setStatusLineDefinition(parseStatusLineDefinition("{Extension:" + name + "}", "", ""));
            text = indicatorText(terminal);
        }
        INFO(std::format("iteration {}: {} (published before: {})", i, text, publishedBefore));
        REQUIRE(text.starts_with(name));
        auto shown = uint64_t { 0 };
        auto const digits = std::string_view { text }.substr(1);
        REQUIRE(std::from_chars(digits.data(), digits.data() + digits.size(), shown).ec == std::errc {});
        CHECK(shown >= publishedBefore);
    }

    stop.store(true);
    publisher.join();
}

TEST_CASE("Terminal.extensions.destroyedWhilePublishing", "[extensions][threading]")
{
    // A worker keeps telling of frames while the terminal is destroyed: the destructor revokes the sink and
    // waits for a delivery in flight, so none reaches a terminal whose members are going.
    auto extensions = FakeStatusLineExtensions {};
    auto stop = std::atomic<bool> { false };
    auto deliveries = std::atomic<int> { 0 };
    auto publisher = std::thread { [&] {
        while (!stop.load())
        {
            extensions.sink.publish();
            ++deliveries;
        }
    } };

    for ([[maybe_unused]] auto const i: std::views::iota(0, 50))
    {
        auto mock = std::make_unique<MockTerm<>>(ExtensionPageSize, extensions.services());
        auto const before = deliveries.load();
        while (deliveries.load() < before + 10)
            std::this_thread::yield();
        mock.reset();
    }

    stop.store(true);
    publisher.join();
    CHECK(deliveries.load() > 0);
}
