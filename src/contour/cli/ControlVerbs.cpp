// SPDX-License-Identifier: Apache-2.0
#include <contour/cli/ControlVerbs.hpp>
#include <contour/cli/HostTransport.hpp>

#include <core/Utf8Path.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <format>
#include <iostream>
#include <iterator>
#include <limits>
#include <ranges>
#include <sstream>
#include <utility>
#include <variant>
#include <vector>

#include <vtcontrol/Events.hpp>
#include <vtcontrol/Json.hpp>
#include <vtcontrol/Wit.hpp>
#include <vthost/client/ControlClient.hpp>

using namespace std::string_literals;
using namespace std::string_view_literals;

namespace CLI = core::cli;

namespace contour::cli
{

namespace
{
    using vthost::client::ControlClientError;
    using vthost::client::ControlFailure;

    /// The reply of a daemon of another release may carry keys this one does not know; reading it for
    /// people must not fail on them, unlike reading a request.
    inline constexpr auto ReplyOptions = glz::opts { .error_on_unknown_keys = false,
                                                     .skip_null_members = true,
                                                     .error_on_missing_keys = false };

    /// @param text A reply's JSON.
    /// @return The reply as @p T, or nothing when it does not read as one.
    template <typename T>
    [[nodiscard]] std::optional<T> decodeReply(std::string_view text)
    {
        auto value = T {};
        if (glz::read<ReplyOptions>(value, text))
            return std::nullopt;
        return value;
    }

    /// Reads a verb's flags under the dotted prefix the command parser gives them.
    class VerbFlags
    {
      public:
        VerbFlags(CLI::FlagStore const& store, std::string_view path):
            _store { store }, _prefix { std::format("contour.{}.", path) }
        {
        }

        [[nodiscard]] bool isSet(std::string_view name) const { return _store.boolean(key(name)); }
        [[nodiscard]] std::string const& text(std::string_view name) const { return _store.str(key(name)); }
        [[nodiscard]] unsigned number(std::string_view name) const { return _store.uint(key(name)); }
        [[nodiscard]] double seconds(std::string_view name) const { return _store.real(key(name)); }
        [[nodiscard]] std::vector<std::string_view> const& words() const { return _store.verbatim; }

      private:
        [[nodiscard]] std::string key(std::string_view name) const { return _prefix + std::string { name }; }

        CLI::FlagStore const& _store;
        std::string _prefix;
    };

    using Params = std::expected<std::string, std::string>;

    /// The longest `--timeout` a wait takes, about 115 days: far past any wait a script means, and far
    /// inside what milliseconds hold.
    constexpr auto MaxTimeoutSeconds = 10'000'000;

    /// @param words Trailing command-line words.
    /// @return Them as owned strings.
    [[nodiscard]] std::vector<std::string> toStrings(std::span<std::string_view const> words)
    {
        return words | std::views::transform([](auto word) { return std::string { word }; })
               | std::ranges::to<std::vector>();
    }

    /// @param words Trailing command-line words.
    /// @return Them joined by single spaces.
    [[nodiscard]] std::string joinWords(std::span<std::string_view const> words)
    {
        auto joined = std::string {};
        for (auto const word: words)
        {
            if (!joined.empty())
                joined += ' ';
            joined += word;
        }
        return joined;
    }

    /// @param id An object id such as `pane:018f3c4e-9a1b-7c2d-8e3f-0123456789ad`.
    /// @return Its last 8 hex digits; ids that are not UUIDs are returned as they are.
    [[nodiscard]] std::string shortId(std::string_view id)
    {
        auto const colon = id.find(':');
        auto const body = colon == std::string_view::npos ? id : id.substr(colon + 1);
        auto digits = std::string {};
        std::ranges::copy_if(body, std::back_inserter(digits), [](char c) { return c != '-'; });
        return digits.size() > 8 ? digits.substr(digits.size() - 8) : digits;
    }

    // {{{ Options
    [[nodiscard]] CLI::Option jsonOption()
    {
        return CLI::Option { "json", CLI::Value { false }, "Prints the reply as JSON, as received." };
    }

    /// The options every verb that reaches a daemon takes.
    [[nodiscard]] CLI::OptionList withConnectionOptions(CLI::OptionList options)
    {
        options.emplace_back(jsonOption());
        options.emplace_back(
            CLI::Option { "daemon",
                          CLI::Value { ""s },
                          "The daemon to talk to: a socket path, HOST:PORT, or a host of hosts: by its name "
                          "or an ssh:// URL. Defaults to $CONTOUR_DAEMON, then the default socket.",
                          "PATH|HOST:PORT|NAME|URL" });
        options.emplace_back(CLI::Option { "token-file",
                                           CLI::Value { ""s },
                                           "Reads the token a HOST:PORT daemon expects from FILE.",
                                           "FILE" });
        options.emplace_back(CLI::Option { "tls-ca",
                                           CLI::Value { ""s },
                                           "The CA certificate (PEM) a HOST:PORT daemon is verified against.",
                                           "FILE" });
        return options;
    }

    [[nodiscard]] CLI::Option paneOption()
    {
        return CLI::Option { "pane",
                             CLI::Value { ""s },
                             "The pane: an id, a label, or a position. Defaults to $CONTOUR_PANE.",
                             "P" };
    }

    [[nodiscard]] CLI::Option noFocusOption()
    {
        return CLI::Option { "no-focus", CLI::Value { false }, "Leaves the focus where it is." };
    }
    // }}}

    // {{{ Human output
    /// Prints @p replyJson as is when it is not a @p T.
    template <typename T, typename Printer>
    void printDecoded(std::string_view replyJson, std::ostream& out, Printer const& printer)
    {
        if (auto const reply = decodeReply<T>(replyJson))
            printer(*reply);
        else
            out << replyJson << '\n';
    }

    void printWorkspace(vtcontrol::ListReply const& reply, std::ostream& out)
    {
        for (auto const& window: reply.windows)
        {
            out << std::format("window {}  {}{}\n",
                               window.position,
                               shortId(window.id),
                               window.label.empty() ? ""s : "  " + window.label);
            for (auto const& tab: window.tabs)
            {
                out << std::format("  {} tab {}  {}  {}\n",
                                   tab.isActive ? '*' : ' ',
                                   tab.position,
                                   shortId(tab.id),
                                   tab.label.empty() ? tab.title : tab.label);
                for (auto const& pane: tab.panes)
                {
                    out << std::format("    {} pane  {}  {}  {}x{}",
                                       pane.isActive ? '*' : ' ',
                                       shortId(pane.id),
                                       pane.label.empty() ? pane.title : pane.label,
                                       pane.size.columns,
                                       pane.size.lines);
                    if (!pane.cwd.empty())
                        out << "  " << pane.cwd;
                    out << '\n';
                }
            }
        }
    }

    void humanList(std::string_view replyJson, std::ostream& out)
    {
        printDecoded<vtcontrol::ListReply>(
            replyJson, out, [&](auto const& reply) { printWorkspace(reply, out); });
    }

    /// Prints the id of the pane a reply of type @p T names.
    template <typename T>
    void humanPane(std::string_view replyJson, std::ostream& out)
    {
        printDecoded<T>(replyJson, out, [&](auto const& reply) { out << reply.pane << '\n'; });
    }

    void humanNothing(std::string_view /*replyJson*/, std::ostream& /*out*/)
    {
    }

    void humanCapture(std::string_view replyJson, std::ostream& out)
    {
        printDecoded<vtcontrol::CaptureReply>(replyJson, out, [&](auto const& reply) { out << reply.text; });
    }

    void humanJson(std::string_view replyJson, std::ostream& out)
    {
        out << replyJson << '\n';
    }

    /// Prints the control WIT package as the checked-in `wit/control/world.wit` holds it, byte for byte, so
    /// that redirecting it into that file regenerates it.
    void humanWit(std::string_view /*replyJson*/, std::ostream& out)
    {
        out << vtcontrol::renderControlWit();
    }

    void humanApiList(std::string_view replyJson, std::ostream& out)
    {
        printDecoded<vtcontrol::ApiListReply>(replyJson, out, [&](auto const& reply) {
            auto const nameWidth = [](auto const& entries) {
                auto width = std::size_t { 0 };
                for (auto const& entry: entries)
                    width = std::max(width, entry.name.size());
                return width;
            };
            auto const width = std::max(nameWidth(reply.methods), nameWidth(reply.events));
            auto const section = [&](std::string_view title, auto const& entries) {
                out << title << ":\n";
                for (auto const& entry: entries)
                    out << std::format("  {:<{}}  {}\n", entry.name, width, entry.summary);
            };
            section("methods", reply.methods);
            out << '\n';
            section("events", reply.events);
        });
    }

    void briefApiList(std::string_view replyJson, std::ostream& out)
    {
        printDecoded<vtcontrol::ApiListReply>(replyJson, out, [&](auto const& reply) {
            for (auto const& entry: reply.methods)
                out << entry.name << '\n';
            for (auto const& entry: reply.events)
                out << entry.name << '\n';
        });
    }

    /// @param block A command block.
    /// @return Its exit code for a table cell: "-" while it has none.
    [[nodiscard]] std::string exitCell(vtcontrol::BlockInfo const& block)
    {
        return block.exitCode ? std::to_string(*block.exitCode) : "-"s;
    }

    /// @param durationMs How long a command ran, if known.
    /// @return "-", "850ms" or "1.2s".
    [[nodiscard]] std::string durationCell(std::optional<uint32_t> durationMs)
    {
        if (!durationMs)
            return "-"s;
        return *durationMs < 1000 ? std::format("{}ms", *durationMs)
                                  : std::format("{:.1f}s", *durationMs / 1000.0);
    }

    void humanBlockList(std::string_view replyJson, std::ostream& out)
    {
        printDecoded<vtcontrol::BlockListReply>(replyJson, out, [&](auto const& reply) {
            for (auto const& block: reply.blocks)
                out << std::format("{}  {:>4}  {:>7}  {}\n",
                                   block.id,
                                   exitCell(block),
                                   durationCell(block.durationMs),
                                   block.command.value_or("-"));
        });
    }

    void humanBlockRead(std::string_view replyJson, std::ostream& out)
    {
        printDecoded<vtcontrol::BlockReadReply>(
            replyJson, out, [&](auto const& reply) { out << reply.prompt << reply.output; });
    }

    /// Notes on @p err what a finished block's status means, and maps the status to the process' exit code.
    /// @tparam T A reply type that may carry the finished block.
    template <typename T>
    int blockExitStatus(std::string_view replyJson, std::ostream& err)
    {
        auto const reply = decodeReply<T>(replyJson);
        // A reply that does not decode says nothing about the block: reporting success would be a false
        // green in the one verb whose exit status is the product.
        if (!reply)
        {
            err << "Error: the daemon's reply could not be read: " << replyJson << '\n';
            return exitCode(ExitStatus::Failed);
        }
        if (!reply->block)
            return exitCode(ExitStatus::Ok);
        if (auto const note = exitStatusNote(*reply->block))
            err << *note << '\n';
        return exitStatusForBlock(*reply->block);
    }

    /// @param payload An event's payload.
    /// @return What to print after the event's type and pane.
    [[nodiscard]] std::string eventDetails(vtcontrol::EventPayload const& payload)
    {
        return std::visit(
            [](auto const& event) -> std::string {
                using Event = std::remove_cvref_t<decltype(event)>;
                if constexpr (std::same_as<Event, vtcontrol::BlockFinishedEvent>)
                    return std::format(
                        "exit {} {}", exitCell(event.block), event.block.command.value_or(""s));
                else if constexpr (std::same_as<Event, vtcontrol::TitleChangedEvent>)
                    return event.title;
                else if constexpr (std::same_as<Event, vtcontrol::CwdChangedEvent>)
                    return event.cwd;
                else if constexpr (std::same_as<Event, vtcontrol::ProgramStatusChangedEvent>)
                    return std::format(
                        "{} record(s), {}", event.records.size(), vtcontrol::nameOf(event.reason));
                else if constexpr (std::same_as<Event, vtcontrol::ClientAttachedEvent>
                                   || std::same_as<Event, vtcontrol::ClientDetachedEvent>)
                    return event.client;
                else if constexpr (std::same_as<Event, vtcontrol::EventsDroppedEvent>)
                    return std::format("{} dropped", event.count);
                else
                    return {};
            },
            payload);
    }

    /// Prints one event as `type pane details`; an event this release cannot read, say one a newer daemon
    /// sends, is printed raw.
    void humanEvent(std::string_view eventJson, std::ostream& out)
    {
        auto const event = vtcontrol::decodeEvent(eventJson);
        if (!event)
        {
            out << eventJson << '\n';
            return;
        }
        auto const pane = event->origin.pane.empty() ? "-"s : "\u2026" + shortId(event->origin.pane);
        auto const details = eventDetails(event->payload);
        out << vtcontrol::eventName(event->payload) << ' ' << pane << (details.empty() ? "" : " ") << details
            << '\n';
    }

    // }}}

    // {{{ Request builders
    [[nodiscard]] Params buildNothing(CLI::FlagStore const& /*flags*/)
    {
        return "{}"s;
    }

    [[nodiscard]] Params buildList(CLI::FlagStore const& flags)
    {
        return vtcontrol::encodeJson(
            vtcontrol::ListRequest { .window = VerbFlags { flags, "ls" }.text("window") });
    }

    [[nodiscard]] Params buildNew(CLI::FlagStore const& flags)
    {
        auto const f = VerbFlags { flags, "new" };
        if (!f.text("window").empty() && f.isSet("new-window"))
            return std::unexpected("--window and --new-window exclude each other"s);
        return vtcontrol::encodeJson(vtcontrol::CreateTabRequest {
            .window = f.text("window"),
            .placement = f.isSet("new-window") ? vtcontrol::WindowPlacement::New
                                               : vtcontrol::WindowPlacement::Existing,
            .cwd = f.text("cwd"),
            .command = toStrings(f.words()),
            .label = f.text("label"),
            .focus = f.isSet("no-focus") ? vtcontrol::FocusPolicy::Keep : vtcontrol::FocusPolicy::Take });
    }

    /// @param text The value of `--ratio`; empty for none.
    /// @return The percentage, 1 to 99, or why it is not one.
    [[nodiscard]] std::expected<std::optional<uint8_t>, std::string> parseRatio(std::string_view text)
    {
        if (text.empty())
            return std::optional<uint8_t> {};
        auto value = 0U;
        auto const [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error != std::errc {} || end != text.data() + text.size() || value < 1 || value > 99)
            return std::unexpected(std::format("--ratio takes a percentage from 1 to 99, not '{}'", text));
        return std::optional { static_cast<uint8_t>(value) };
    }

    [[nodiscard]] Params buildSplit(CLI::FlagStore const& flags)
    {
        auto const f = VerbFlags { flags, "split" };
        if (f.isSet("right") && f.isSet("below"))
            return std::unexpected("--right and --below exclude each other"s);
        auto const ratio = parseRatio(f.text("ratio"));
        if (!ratio)
            return std::unexpected(ratio.error());
        return vtcontrol::encodeJson(vtcontrol::SplitRequest {
            .pane = f.text("pane"),
            .direction =
                f.isSet("below") ? vtcontrol::SplitDirection::Below : vtcontrol::SplitDirection::Right,
            .ratioPercent = *ratio,
            .cwd = f.text("cwd"),
            .command = toStrings(f.words()),
            .label = f.text("label"),
            .focus = f.isSet("no-focus") ? vtcontrol::FocusPolicy::Keep : vtcontrol::FocusPolicy::Take });
    }

    [[nodiscard]] Params buildSend(CLI::FlagStore const& flags)
    {
        auto const f = VerbFlags { flags, "send" };
        auto text = joinWords(f.words());
        if (f.isSet("enter"))
            text += '\r';
        return vtcontrol::encodeJson(vtcontrol::SendRequest {
            .pane = f.text("pane"),
            .text = std::move(text),
            .wait = f.isSet("wait") ? vtcontrol::SendWait::CommandFinished : vtcontrol::SendWait::None });
    }

    [[nodiscard]] Params buildCapture(CLI::FlagStore const& flags)
    {
        auto const f = VerbFlags { flags, "pane.capture" };
        if (f.isSet("all") && f.number("scrollback") != 0)
            return std::unexpected("--scrollback and --all exclude each other"s);
        // Past int32_t's range the cast would wrap, and 4294967295 would read as --all.
        if (std::cmp_greater(f.number("scrollback"), std::numeric_limits<int32_t>::max()))
            return std::unexpected(std::format("--scrollback takes at most {} lines; --all takes all of them",
                                               std::numeric_limits<int32_t>::max()));
        auto const scrollback = f.isSet("all") ? -1 : static_cast<int32_t>(f.number("scrollback"));
        return vtcontrol::encodeJson(
            vtcontrol::CaptureRequest { .pane = f.text("pane"), .scrollback = scrollback });
    }

    [[nodiscard]] Params buildClose(CLI::FlagStore const& flags)
    {
        auto const f = VerbFlags { flags, "close" };
        if (!f.text("pane").empty() && !f.text("tab").empty())
            return std::unexpected("--pane and --tab exclude each other"s);
        if (!f.text("tab").empty())
            return vtcontrol::encodeJson(vtcontrol::CloseTabRequest { .tab = f.text("tab") });
        return vtcontrol::encodeJson(vtcontrol::ClosePaneRequest { .pane = f.text("pane") });
    }

    [[nodiscard]] bool closesTab(CLI::FlagStore const& flags)
    {
        return !VerbFlags { flags, "close" }.text("tab").empty();
    }

    [[nodiscard]] Params buildLabel(CLI::FlagStore const& flags)
    {
        auto const& words = VerbFlags { flags, "label" }.words();
        if (words.size() != 2)
            return std::unexpected("label takes an OBJECT and a LABEL (an empty one clears it)"s);
        return vtcontrol::encodeJson(vtcontrol::SetLabelRequest { .object = std::string { words[0] },
                                                                  .label = std::string { words[1] } });
    }

    [[nodiscard]] Params buildApiDescribe(CLI::FlagStore const& flags)
    {
        auto const& words = VerbFlags { flags, "api.describe" }.words();
        if (words.size() != 1)
            return std::unexpected("api describe takes exactly one NAME"s);
        return vtcontrol::encodeJson(vtcontrol::ApiDescribeRequest { .name = std::string { words[0] } });
    }

    [[nodiscard]] Params buildApiCall(CLI::FlagStore const& flags)
    {
        auto const& words = VerbFlags { flags, "api.call" }.words();
        if (words.empty() || words.size() > 2)
            return std::unexpected("api call takes a NAME and optionally a JSON document"s);
        // The name is the method (MethodSource::FirstWord); the params are what follows it.
        return std::string { words.size() == 2 ? words[1] : "{}"sv };
    }

    [[nodiscard]] Params buildBlockList(CLI::FlagStore const& flags)
    {
        auto const f = VerbFlags { flags, "block.list" };
        return vtcontrol::encodeJson(
            vtcontrol::BlockListRequest { .pane = f.text("pane"), .limit = f.number("limit") });
    }

    /// @param words A verb's trailing words.
    /// @return The one block it names, empty for none, or why it names several.
    [[nodiscard]] std::expected<std::string, std::string> blockWord(std::span<std::string_view const> words)
    {
        if (words.size() > 1)
            return std::unexpected("at most one BLOCK may be named"s);
        return words.empty() ? std::string {} : std::string { words.front() };
    }

    [[nodiscard]] Params buildBlockRead(CLI::FlagStore const& flags)
    {
        auto const f = VerbFlags { flags, "block.read" };
        if (f.isSet("prompt") && f.isSet("output"))
            return std::unexpected("--prompt and --output exclude each other"s);
        auto const block = blockWord(f.words());
        if (!block)
            return std::unexpected(block.error());
        auto part = vtcontrol::BlockPart::All;
        if (f.isSet("prompt"))
            part = vtcontrol::BlockPart::Prompt;
        else if (f.isSet("output"))
            part = vtcontrol::BlockPart::Output;
        return vtcontrol::encodeJson(
            vtcontrol::BlockReadRequest { .block = *block, .pane = f.text("pane"), .part = part });
    }

    [[nodiscard]] Params buildWait(CLI::FlagStore const& flags)
    {
        auto const f = VerbFlags { flags, "wait" };
        // Bounded here, before anything converts it: a float past what an integer holds (1e300, inf, nan)
        // makes the conversion to milliseconds undefined.
        if (auto const seconds = f.seconds("timeout");
            !std::isfinite(seconds) || seconds < 0.0 || seconds > double { MaxTimeoutSeconds })
            return std::unexpected(
                std::format("--timeout takes a number of seconds up to {}, 0 for none", MaxTimeoutSeconds));
        auto const block = blockWord(f.words());
        if (!block)
            return std::unexpected(block.error());
        return vtcontrol::encodeJson(vtcontrol::WaitRequest { .block = *block, .pane = f.text("pane") });
    }

    [[nodiscard]] Params buildEvents(CLI::FlagStore const& flags)
    {
        auto const f = VerbFlags { flags, "events" };
        auto kinds = std::vector<std::string> {};
        for (auto const kind: std::views::split(std::string_view { f.text("kind") }, ',')
                                  | std::views::transform([](auto part) { return std::string_view { part }; })
                                  | std::views::filter([](auto part) { return !part.empty(); }))
            kinds.emplace_back(kind);
        return vtcontrol::encodeJson(
            vtcontrol::SubscribeRequest { .kinds = std::move(kinds), .pane = f.text("pane") });
    }

    // }}}

    /// The row table. A function-local static: Verbatim owns strings, which a constant cannot.
    [[nodiscard]] std::vector<ControlVerb> const& verbTable()
    {
        static auto const table = std::vector<ControlVerb> {
            ControlVerb {
                .path = "ls",
                .method = "workspace.list",
                .usage = "ls [--window W]",
                .summary = "Lists the windows, tabs and panes; * marks the active ones.",
                .options =
                    [] {
                        return withConnectionOptions(
                            { CLI::Option { "window", CLI::Value { ""s }, "Lists this window only.", "W" } });
                    },
                .verbatim = std::nullopt,
                .buildParams = buildList,
                .printHuman = humanList,
            },
            ControlVerb {
                .path = "new",
                .method = "tab.create",
                .usage = "new [--window W | --new-window] [--cwd DIR] [--label L] [--no-focus] [-- CMD...]",
                .summary = "Opens a tab and prints its pane id.",
                .options =
                    [] {
                        return withConnectionOptions({
                            CLI::Option {
                                "window", CLI::Value { ""s }, "Opens the tab in this window.", "W" },
                            CLI::Option {
                                "new-window", CLI::Value { false }, "Opens the tab in a new window." },
                            CLI::Option { "cwd", CLI::Value { ""s }, "The working directory.", "DIR" },
                            CLI::Option { "label", CLI::Value { ""s }, "Labels the new tab.", "L" },
                            noFocusOption(),
                        });
                    },
                .verbatim = CLI::Verbatim { "CMD", "The program and arguments to run instead of the shell." },
                .buildParams = buildNew,
                .printHuman = humanPane<vtcontrol::CreateTabReply>,
            },
            ControlVerb {
                .path = "split",
                .method = "pane.split",
                .usage = "split [--pane P] [--right | --below] [--ratio PCT] [--cwd DIR] [--label L] "
                         "[--no-focus] [-- CMD...]",
                .summary = "Splits a pane and prints the new pane's id.",
                .options =
                    [] {
                        return withConnectionOptions({
                            paneOption(),
                            CLI::Option { "right", CLI::Value { false }, "Puts the new pane to the right." },
                            CLI::Option { "below", CLI::Value { false }, "Puts the new pane below." },
                            CLI::Option { "ratio",
                                          CLI::Value { ""s },
                                          "The share of the old pane the new one gets, 1 to 99.",
                                          "PCT" },
                            CLI::Option { "cwd", CLI::Value { ""s }, "The working directory.", "DIR" },
                            CLI::Option { "label", CLI::Value { ""s }, "Labels the new pane.", "L" },
                            noFocusOption(),
                        });
                    },
                .verbatim = CLI::Verbatim { "CMD", "The program and arguments to run instead of the shell." },
                .buildParams = buildSplit,
                .printHuman = humanPane<vtcontrol::SplitReply>,
            },
            ControlVerb {
                .path = "send",
                .method = "pane.send",
                .usage = "send [--pane P] [--enter] [--wait] TEXT...|-",
                .summary = "Types text into a pane; --wait exits with the command's own status.",
                .options =
                    [] {
                        return withConnectionOptions({
                            paneOption(),
                            CLI::Option { "enter", CLI::Value { false }, "Appends a carriage return." },
                            CLI::Option {
                                "wait",
                                CLI::Value { false },
                                "Waits for the command the text starts, and exits with its status." },
                        });
                    },
                .verbatim = CLI::Verbatim { "TEXT",
                                            "The words to type, joined by spaces; - reads standard input. "
                                            "Put -- before text that starts with an option's name." },
                .buildParams = buildSend,
                .printHuman = humanNothing,
                .input = InputExpansion::Stdin,
                .replyExitStatus = blockExitStatus<vtcontrol::SendReply>,
            },
            ControlVerb {
                .path = "pane.capture",
                .method = "pane.capture",
                .usage = "pane capture [--pane P] [--scrollback N | --all]",
                .summary = "Prints a pane's text.",
                .options =
                    [] {
                        return withConnectionOptions({
                            paneOption(),
                            CLI::Option { "scrollback",
                                          CLI::Value { 0U },
                                          "Includes N history lines above the page.",
                                          "N" },
                            CLI::Option { "all", CLI::Value { false }, "Includes all of the history." },
                        });
                    },
                .verbatim = std::nullopt,
                .buildParams = buildCapture,
                .printHuman = humanCapture,
            },
            ControlVerb {
                .path = "close",
                .method = "pane.close",
                .usage = "close [--pane P | --tab T]",
                .summary = "Closes a pane, or a tab with all its panes.",
                .options =
                    [] {
                        return withConnectionOptions({
                            paneOption(),
                            CLI::Option { "tab", CLI::Value { ""s }, "Closes this tab instead.", "T" },
                        });
                    },
                .verbatim = std::nullopt,
                .buildParams = buildClose,
                .printHuman = humanNothing,
                .alternateMethod = "tab.close",
                .usesAlternateMethod = closesTab,
            },
            ControlVerb {
                .path = "label",
                .method = "object.setLabel",
                .usage = "label OBJECT LABEL",
                .summary = "Labels a window, tab or pane to address it by; an empty label clears it.",
                .options = [] { return withConnectionOptions({}); },
                .verbatim =
                    CLI::Verbatim { "OBJECT LABEL", "The object, e.g. pane:logs, and its new label." },
                .buildParams = buildLabel,
                .printHuman = humanNothing,
            },
            ControlVerb {
                .path = "api.list",
                .method = "api.list",
                .usage = "api list [-q]",
                .summary = "Lists the methods and events of the control API.",
                .options =
                    [] {
                        return withConnectionOptions({ CLI::Option { CLI::OptionName { 'q', "quiet" },
                                                                     CLI::Value { false },
                                                                     "Prints the names only." } });
                    },
                .verbatim = std::nullopt,
                .buildParams = buildNothing,
                .printHuman = humanApiList,
                .briefFlag = "quiet",
                .printBrief = briefApiList,
            },
            ControlVerb {
                .path = "api.describe",
                .method = "api.describe",
                .usage = "api describe NAME",
                .summary = "Describes one method or event: its schemas and examples.",
                .options = [] { return withConnectionOptions({}); },
                .verbatim = CLI::Verbatim { "NAME", "The method or event." },
                .buildParams = buildApiDescribe,
                .printHuman = humanJson,
                .human = HumanForm::Json,
            },
            ControlVerb {
                .path = "api.call",
                .method = "",
                .usage = "api call NAME [JSON|@FILE|-]",
                .summary = "Calls any method with the given request JSON, sent as is and without context.",
                .options = [] { return withConnectionOptions({}); },
                .verbatim = CLI::Verbatim { "NAME [JSON|@FILE|-]",
                                            "The method, and its request as JSON, @FILE or - for standard "
                                            "input; {} by default." },
                .buildParams = buildApiCall,
                .printHuman = humanJson,
                .methodSource = MethodSource::FirstWord,
                .context = ContextPolicy::Omit,
                .human = HumanForm::Json,
                .input = InputExpansion::StdinOrFile,
            },
            ControlVerb {
                .path = "api.wit",
                .method = "",
                .usage = "api wit",
                .summary = "Prints the control API as the WIT package a WebAssembly extension imports.",
                .options = [] { return CLI::OptionList {}; },
                .verbatim = std::nullopt,
                .buildParams = buildNothing,
                .printHuman = humanWit,
            },
            ControlVerb {
                .path = "block.list",
                .method = "block.list",
                .usage = "block list [--pane P] [--limit N]",
                .summary = "Lists a pane's recent command blocks, newest first.",
                .options =
                    [] {
                        return withConnectionOptions({
                            paneOption(),
                            CLI::Option { "limit", CLI::Value { 20U }, "At most N blocks.", "N" },
                        });
                    },
                .verbatim = std::nullopt,
                .buildParams = buildBlockList,
                .printHuman = humanBlockList,
            },
            ControlVerb {
                .path = "block.read",
                .method = "block.read",
                .usage = "block read [BLOCK] [--pane P] [--prompt | --output]",
                .summary = "Prints a command block's text; without BLOCK, the pane's last finished one.",
                .options =
                    [] {
                        return withConnectionOptions({
                            paneOption(),
                            CLI::Option { "prompt", CLI::Value { false }, "Prints the prompt only." },
                            CLI::Option { "output", CLI::Value { false }, "Prints the output only." },
                        });
                    },
                .verbatim =
                    CLI::Verbatim { "BLOCK", "The block's id; the pane's last finished block by default." },
                .buildParams = buildBlockRead,
                .printHuman = humanBlockRead,
            },
            ControlVerb {
                .path = "wait",
                .method = "block.wait",
                .usage = "wait [BLOCK] [--pane P] [--timeout SECONDS]",
                .summary = "Waits for a command block to finish and exits with the command's own status.",
                .options =
                    [] {
                        return withConnectionOptions({
                            paneOption(),
                            CLI::Option { "timeout",
                                          CLI::Value { 0.0 },
                                          "Gives up after SECONDS (exit status 1, \"timed out\"); 0 waits "
                                          "for as long as it takes.",
                                          "SECONDS" },
                        });
                    },
                .verbatim =
                    CLI::Verbatim { "BLOCK", "The block's id; the pane's next block to finish by default." },
                .buildParams = buildWait,
                .printHuman = humanNothing,
                .replyExitStatus = blockExitStatus<vtcontrol::WaitReply>,
                .timeoutFlag = "timeout",
            },
            ControlVerb {
                .path = "events",
                .method = "events.subscribe",
                .usage = "events [--follow] [--kind K[,K...]] [--pane P]",
                .summary = "Prints the first matching event, or with --follow every one until Ctrl+C.",
                .options =
                    [] {
                        return withConnectionOptions({
                            paneOption(),
                            CLI::Option {
                                "follow", CLI::Value { false }, "Keeps printing events until Ctrl+C." },
                            CLI::Option {
                                "kind",
                                CLI::Value { ""s },
                                "Only these events, a comma-separated list such as block.finished,pane.bell.",
                                "K" },
                        });
                    },
                .verbatim = std::nullopt,
                .buildParams = buildEvents,
                .printHuman = humanEvent,
                .mode = VerbMode::Stream,
                .followFlag = "follow",
            },
        };
        return table;
    }
} // namespace

namespace
{
    /// The channel that dials a transport for every call.
    class TransportChannel final: public ControlChannel
    {
      public:
        explicit TransportChannel(std::shared_ptr<vthost::ITransportFactory> transport):
            _transport { std::move(transport) }
        {
        }

        [[nodiscard]] std::expected<std::string, ControlClientError> call(
            std::string_view method,
            std::string_view params,
            std::string_view context,
            std::optional<std::chrono::milliseconds> timeout) override
        {
            return named(vthost::client::runControlCall(*_transport, method, params, context, timeout));
        }

        [[nodiscard]] std::expected<void, ControlClientError> stream(
            std::string_view params,
            std::string_view context,
            std::function<vthost::client::StreamStep(std::string_view)> onEvent,
            core::async::StopToken const& stop) override
        {
            return named(
                vthost::client::runEventStream(*_transport, params, context, std::move(onEvent), stop));
        }

      private:
        /// Says which daemon an unreachable one was: a bare "connect failed" leaves a script guessing.
        template <typename T>
        [[nodiscard]] std::expected<T, ControlClientError> named(
            std::expected<T, ControlClientError> result) const
        {
            if (!result && result.error().failure == ControlFailure::Unreachable)
                result.error().message = std::format(
                    "cannot reach the daemon at {}: {}", _transport->describe(), result.error().message);
            return result;
        }

        std::shared_ptr<vthost::ITransportFactory> _transport;
    };

    /// Writes a failed call the way every verb does: `Error: <message>`, then an ambiguous target's
    /// candidates one per line.
    void reportFailure(ControlClientError const& error, std::ostream& err)
    {
        err << "Error: " << (error.failure == ControlFailure::TimedOut ? "timed out"s : error.message)
            << '\n';
        if (error.remote)
            for (auto const& candidate: error.remote->candidates)
                err << "  " << candidate << '\n';
    }

    /// @return Exit status of a malformed command line, after saying why.
    [[nodiscard]] int reportBadUsage(std::string_view message, std::ostream& err)
    {
        err << "Error: " << message << '\n';
        return exitCode(ExitStatus::BadUsage);
    }

    [[nodiscard]] bool isLocal(ControlVerb const& verb) noexcept
    {
        return verb.method.empty() && verb.methodSource == MethodSource::Fixed;
    }

    /// @param verb A verb.
    /// @param flags Its flags.
    /// @param io Where standard input and files are read.
    /// @param owned Keeps the text the returned words may point into alive.
    /// @return The flags with the last word read as the verb's InputExpansion says, or why it cannot be.
    [[nodiscard]] std::expected<CLI::FlagStore, std::string> expandInput(ControlVerb const& verb,
                                                                         CLI::FlagStore const& flags,
                                                                         ControlIo const& io,
                                                                         std::string& owned)
    {
        auto expanded = flags;
        auto& words = expanded.verbatim;
        auto const index = verb.input == InputExpansion::Stdin ? std::size_t { 0 } : std::size_t { 1 };
        auto const eligible = verb.input == InputExpansion::Stdin ? words.size() == 1 : words.size() == 2;
        if (verb.input == InputExpansion::None || !eligible)
            return expanded;
        if (words[index] == "-")
        {
            owned.assign(std::istreambuf_iterator<char> { io.in }, {});
            words[index] = owned;
        }
        else if (verb.input == InputExpansion::StdinOrFile && words[index].starts_with('@'))
        {
            auto const name = words[index].substr(1);
            auto const path = core::pathFromUtf8(name);
            if (!path)
                return std::unexpected(std::format("'{}' is not a usable file name", name));
            auto text = io.fs.readFile(*path);
            if (!text)
                return std::unexpected(std::format("cannot read '{}': {}", name, text.error()));
            owned = std::move(*text);
            words[index] = owned;
        }
        return expanded;
    }

    /// @param flags A verb's flags.
    /// @param path The verb's path.
    /// @return The call's time limit, if the verb has one set.
    [[nodiscard]] std::optional<std::chrono::milliseconds> timeoutOf(ControlVerb const& verb,
                                                                     CLI::FlagStore const& flags)
    {
        if (verb.timeoutFlag.empty())
            return std::nullopt;
        auto const seconds = VerbFlags { flags, verb.path }.seconds(verb.timeoutFlag);
        if (seconds <= 0.0)
            return std::nullopt;
        return std::chrono::milliseconds { static_cast<std::chrono::milliseconds::rep>(seconds * 1000.0) };
    }

    /// Prints a reply for people.
    void printReply(ControlVerb const& verb,
                    CLI::FlagStore const& flags,
                    std::string_view reply,
                    ControlIo const& io)
    {
        auto const brief = !verb.briefFlag.empty() && verb.printBrief != nullptr
                           && VerbFlags { flags, verb.path }.isSet(verb.briefFlag);
        if (brief)
            verb.printBrief(reply, io.out);
        else if (verb.human == HumanForm::Json && io.target == OutputTarget::Terminal)
            verb.printHuman(glz::prettify_json(reply), io.out);
        else
            verb.printHuman(reply, io.out);
    }

    /// Subscribes and prints every event until the stream ends: after the first when the verb is not told to
    /// follow, at Ctrl+C otherwise.
    [[nodiscard]] int streamAndPrint(ControlVerb const& verb,
                                     CLI::FlagStore const& flags,
                                     std::string_view params,
                                     std::string_view context,
                                     ControlChannel& channel,
                                     ControlIo const& io)
    {
        auto const f = VerbFlags { flags, verb.path };
        auto const json = f.isSet("json");
        auto const follows = verb.followFlag.empty() || f.isSet(verb.followFlag);
        auto const onEvent = [&](std::string_view eventJson) {
            if (json)
                io.out << eventJson << '\n';
            else
            {
                auto const now = std::chrono::floor<std::chrono::seconds>(io.clock.now());
                io.out << std::format("{:%H:%M:%S} ", now);
                verb.printHuman(eventJson, io.out);
            }
            io.out.flush();
            return follows ? vthost::client::StreamStep::Continue : vthost::client::StreamStep::Stop;
        };
        auto const ended = channel.stream(params, context, onEvent, io.stop);
        if (!ended)
        {
            reportFailure(ended.error(), io.err);
            return exitCode(exitStatusFor(ended.error()));
        }
        return exitCode(io.stop.stop_requested() ? ExitStatus::Interrupted : ExitStatus::Ok);
    }

    /// Calls the verb's method with its prepared params, and prints what comes back.
    [[nodiscard]] int callAndPrint(ControlVerb const& verb,
                                   CLI::FlagStore const& flags,
                                   std::string_view params,
                                   ControlChannel& channel,
                                   ControlIo const& io)
    {
        auto const context =
            verb.context == ContextPolicy::Send ? io.env.get("CONTOUR_PANE").value_or(""s) : ""s;
        if (verb.mode == VerbMode::Stream)
            return streamAndPrint(verb, flags, params, context, channel, io);
        auto const reply = channel.call(methodFor(verb, flags), params, context, timeoutOf(verb, flags));
        if (!reply)
        {
            reportFailure(reply.error(), io.err);
            return exitCode(exitStatusFor(reply.error()));
        }
        if (VerbFlags { flags, verb.path }.isSet("json"))
            io.out << *reply << '\n';
        else
            printReply(verb, flags, *reply, io);
        return verb.replyExitStatus != nullptr ? verb.replyExitStatus(*reply, io.err)
                                               : exitCode(ExitStatus::Ok);
    }

    /// Reads a file named on the command line.
    /// @return Its text, nothing when @p name is empty, or why it cannot be read.
    [[nodiscard]] std::expected<std::string, std::string> readOptionFile(core::platform::FileSystem& fs,
                                                                         std::string_view option,
                                                                         std::string const& name)
    {
        if (name.empty())
            return std::string {};
        auto const path = core::pathFromUtf8(name);
        if (!path)
            return std::unexpected(std::format("--{}: '{}' is not a usable file name", option, name));
        return fs.readFile(*path).transform_error([&](auto const& reason) {
            return std::format("--{}: cannot read '{}': {}", option, name, reason);
        });
    }

    /// @param text A token file's contents.
    /// @return The token: carriage returns and trailing whitespace dropped, inner spaces kept.
    [[nodiscard]] std::string cleanToken(std::string text)
    {
        std::erase(text, '\r');
        while (!text.empty() && (text.back() == '\n' || text.back() == ' ' || text.back() == '\t'))
            text.pop_back();
        return text;
    }
    /// A verb's flags with their input read, and the request they build.
    struct PreparedRequest
    {
        /// Owns the text the words of @ref flags were read from (standard input, a file); on the heap, so
        /// moving the request does not move the characters those words point at.
        std::shared_ptr<std::string> text = std::make_shared<std::string>();
        CLI::FlagStore flags; ///< The flags, the last word expanded.
        std::string params;   ///< The request JSON.
    };

    /// @return The prepared request, or the exit status after saying why the command line is malformed.
    [[nodiscard]] std::expected<PreparedRequest, int> prepareRequest(ControlVerb const& verb,
                                                                     CLI::FlagStore const& flags,
                                                                     ControlIo const& io)
    {
        auto prepared = PreparedRequest {};
        auto expanded = expandInput(verb, flags, io, *prepared.text);
        if (!expanded)
            return std::unexpected(reportBadUsage(expanded.error(), io.err));
        auto params = verb.buildParams(*expanded);
        if (!params)
            return std::unexpected(reportBadUsage(params.error(), io.err));
        prepared.flags = *std::move(expanded);
        prepared.params = *std::move(params);
        return prepared;
    }

    /// Runs a verb that never reaches a daemon.
    [[nodiscard]] int runLocal(ControlVerb const& verb, ControlIo const& io)
    {
        verb.printHuman("", io.out);
        return exitCode(ExitStatus::Ok);
    }
} // namespace

std::unique_ptr<ControlChannel> makeTransportChannel(std::shared_ptr<vthost::ITransportFactory> transport)
{
    return std::make_unique<TransportChannel>(std::move(transport));
}

vthost::client::SavedHostResolver makeSavedHostResolver(HostLookup lookup,
                                                        core::platform::FileSystem const& fileSystem,
                                                        std::shared_ptr<vthost::IStdioSpawner> spawner)
{
    return
        [lookup = std::move(lookup), &fileSystem, spawner = std::move(spawner)](
            std::string_view spec) -> std::expected<std::shared_ptr<vthost::ITransportFactory>, std::string> {
            return lookup(spec)
                .and_then([&](config::HostEntry const& host) {
                    return makeHostTransport(std::string { spec }, host, fileSystem, spawner);
                })
                .transform([](HostTransport transport) { return std::move(transport.factory); })
                .transform_error(
                    [&](std::string const& why) { return std::format("--daemon {}: {}", spec, why); });
        };
}

ExitStatus exitStatusFor(vtcontrol::ControlErrorCode code) noexcept
{
    using vtcontrol::ControlErrorCode;
    switch (code)
    {
        case ControlErrorCode::BadRequest: return ExitStatus::BadUsage;
        case ControlErrorCode::NotFound:
        case ControlErrorCode::Ambiguous: return ExitStatus::TargetNotFound;
        case ControlErrorCode::MethodNotFound:
        case ControlErrorCode::Unsupported:
        case ControlErrorCode::Forbidden:
        case ControlErrorCode::Failed:
        case ControlErrorCode::LimitReached: break;
    }
    return ExitStatus::Failed;
}

ExitStatus exitStatusFor(ControlFailure failure) noexcept
{
    switch (failure)
    {
        case ControlFailure::Unreachable:
        case ControlFailure::Disconnected: return ExitStatus::Unreachable;
        case ControlFailure::Misconfigured: return ExitStatus::BadUsage;
        case ControlFailure::Refused:
        case ControlFailure::Unsupported:
        case ControlFailure::Untrusted:
        case ControlFailure::TimedOut:
        case ControlFailure::Remote: break;
    }
    return ExitStatus::Failed;
}

ExitStatus exitStatusFor(ControlClientError const& error) noexcept
{
    if (error.failure == ControlFailure::Remote && error.remote)
        return exitStatusFor(error.remote->code);
    return exitStatusFor(error.failure);
}

int exitStatusForBlock(vtcontrol::BlockInfo const& block) noexcept
{
    if (block.state == vtcontrol::BlockState::Superseded || !block.exitCode || *block.exitCode < 0)
        return exitCode(ExitStatus::Failed);
    return std::min(*block.exitCode, int32_t { 255 });
}

std::optional<std::string> exitStatusNote(vtcontrol::BlockInfo const& block)
{
    if (block.state == vtcontrol::BlockState::Superseded)
        return "note: the command's block was superseded; exiting 1"s;
    if (!block.exitCode)
        return "note: the command reported no exit code; exiting 1"s;
    if (*block.exitCode < 0)
        return std::format("note: the command's exit code is {}; exiting 1", *block.exitCode);
    return std::nullopt;
}

std::span<ControlVerb const> controlVerbs()
{
    return verbTable();
}

std::span<ControlVerbGroup const> controlVerbGroups()
{
    static constexpr auto Groups = std::array {
        ControlVerbGroup { "pane", "Reads panes." },
        ControlVerbGroup { "api", "Explores and calls the control API." },
    };
    return Groups;
}

std::string_view methodFor(ControlVerb const& verb, CLI::FlagStore const& flags)
{
    if (verb.methodSource == MethodSource::FirstWord)
    {
        auto const& words = flags.verbatim;
        return words.empty() ? std::string_view {} : words.front();
    }
    if (verb.usesAlternateMethod != nullptr && verb.usesAlternateMethod(flags))
        return verb.alternateMethod;
    return verb.method;
}

int runControlVerb(ControlVerb const& verb,
                   CLI::FlagStore const& flags,
                   ControlChannel& channel,
                   ControlIo const& io)
{
    auto prepared = prepareRequest(verb, flags, io);
    if (!prepared)
        return prepared.error();
    if (isLocal(verb))
        return runLocal(verb, io);
    return callAndPrint(verb, prepared->flags, prepared->params, channel, io);
}

int runControlVerb(ControlVerb const& verb, CLI::FlagStore const& flags, ControlIo const& io)
{
    auto prepared = prepareRequest(verb, flags, io);
    if (!prepared)
        return prepared.error();
    if (isLocal(verb))
        return runLocal(verb, io);

    auto const f = VerbFlags { flags, verb.path };
    auto const token = readOptionFile(io.fs, "token-file", f.text("token-file"));
    auto const authority = readOptionFile(io.fs, "tls-ca", f.text("tls-ca"));
    if (!token || !authority)
        return reportBadUsage(!token ? token.error() : authority.error(), io.err);
    auto const transport = vthost::client::resolveControlEndpoint(
        vthost::client::ControlEndpointOptions { .daemon = f.text("daemon"),
                                                 .token = cleanToken(*token),
                                                 .tlsCaPem = *authority,
                                                 .resolveSavedHost = io.resolveSavedHost },
        io.env);
    if (!transport)
    {
        reportFailure(transport.error(), io.err);
        return exitCode(exitStatusFor(transport.error()));
    }
    auto channel = makeTransportChannel(*transport);
    return callAndPrint(verb, prepared->flags, prepared->params, *channel, io);
}

} // namespace contour::cli
