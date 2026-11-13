// SPDX-License-Identifier: Apache-2.0
#include <contour/cli/TerminalPinPrompt.hpp>

#include <core/async/ExecutorContext.hpp>
#include <core/platform/Wakeup.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <ostream>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>

#include <vthost/PinAnswer.hpp>

// clang-format off
#ifdef _WIN32
    #include <windows.h>

    #include <core/Utils.hpp>
#else
    #include <cerrno>
    #include <poll.h>
    #include <unistd.h>
#endif
// clang-format on

namespace contour::cli
{

namespace
{
    /// What a user may answer to trust the daemon, lower case.
    constexpr auto TrustAnswers = std::array<std::string_view, 2> { "y", "yes" };

    /// @param answer What the user typed.
    /// @return Whether it says yes: a whole word of TrustAnswers, whatever its case and the blanks around.
    [[nodiscard]] bool saysYes(std::string_view answer)
    {
        auto const blank = [](unsigned char ch) {
            return std::isspace(ch) != 0;
        };
        while (!answer.empty() && blank(static_cast<unsigned char>(answer.front())))
            answer.remove_prefix(1);
        while (!answer.empty() && blank(static_cast<unsigned char>(answer.back())))
            answer.remove_suffix(1);
        return std::ranges::any_of(TrustAnswers, [&](std::string_view yes) {
            return std::ranges::equal(
                answer, yes, [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == b; });
        });
    }
} // namespace

TerminalAttendance currentTerminalAttendance() noexcept
{
#ifdef _WIN32
    // Both must be a console: a pipe or a file has no one at it, and its reads are not the console's.
    auto const isConsole = [](DWORD which) {
        auto const handle = ::GetStdHandle(which);
        auto mode = DWORD {};
        return handle != nullptr && handle != INVALID_HANDLE_VALUE && ::GetConsoleMode(handle, &mode) != 0;
    };
    auto const attended = isConsole(STD_INPUT_HANDLE) && isConsole(STD_ERROR_HANDLE);
    return attended ? TerminalAttendance::Attended : TerminalAttendance::Unattended;
#else
    auto const attended = isatty(STDIN_FILENO) != 0 && isatty(STDERR_FILENO) != 0;
    return attended ? TerminalAttendance::Attended : TerminalAttendance::Unattended;
#endif
}

TerminalPinPrompt::TerminalPinPrompt(core::platform::NativeHandle input, std::ostream& out):
    _input { input }, _out { out }
{
}

namespace
{
    /// The longest answer read; whatever is typed past it is no answer to y/N.
    constexpr auto MaxLine = std::size_t { 256 };

    /// Where the reader thread is in reading a line, and whether it was told to stop. A state of its own, not
    /// a flag: what the reader does and what the destructor does about it must be decided by one exchange.
    enum class ReaderPhase : std::uint8_t
    {
        Idle = 0, ///< Not in a read that only an Enter ends: nothing to type for it.
        Reading,  ///< In such a read (Windows): the console returns from it only once an Enter is typed.
        Stopped,  ///< The LineReader is ending: no line is begun from here on.
    };

    /// What the reader thread works on, shared with it.
    struct ReaderState
    {
        /// @param inputHandle What is read; borrowed.
        /// @param answerSender Told what the user decided, if they did.
        ReaderState(core::platform::NativeHandle inputHandle,
                    std::shared_ptr<vthost::PinAnswer::Sender> answerSender):
            input { inputHandle }, sender { std::move(answerSender) }
        {
        }

        core::platform::NativeHandle input;
        std::shared_ptr<vthost::PinAnswer::Sender> sender;
        core::platform::Wakeup wakeup; ///< Signalled to stop the reader.
        /// Moved from Idle to Reading by the reader, with a compare-exchange, before a console read that only
        /// an Enter ends; back to Idle when it ends. Moved to Stopped by the destructor, which types an Enter
        /// for the read only if the state it replaced was Reading. Windows only: the POSIX reader polls.
        std::atomic<ReaderPhase> phase = ReaderPhase::Idle;
    };

#ifdef _WIN32
    /// Types an Enter into the console's input, as the user would: a console read that has begun a line
    /// returns only then, and this is how it is made to return when its answer is no longer wanted.
    /// @return Whether both key events were written.
    [[nodiscard]] bool injectEnter(ReaderState const& state)
    {
        auto records = std::array<INPUT_RECORD, 2> {};
        auto isDown = TRUE;
        for (auto& record: records)
        {
            record.EventType = KEY_EVENT;
            record.Event.KeyEvent.bKeyDown = isDown;
            record.Event.KeyEvent.wRepeatCount = 1;
            record.Event.KeyEvent.wVirtualKeyCode = VK_RETURN;
            record.Event.KeyEvent.wVirtualScanCode =
                static_cast<WORD>(::MapVirtualKeyW(VK_RETURN, MAPVK_VK_TO_VSC));
            record.Event.KeyEvent.uChar.UnicodeChar = L'\r';
            record.Event.KeyEvent.dwControlKeyState = 0;
            isDown = FALSE;
        }
        auto written = DWORD {};
        return ::WriteConsoleInputW(state.input, records.data(), static_cast<DWORD>(records.size()), &written)
                   != 0
               && written == static_cast<DWORD>(records.size());
    }

    /// Makes a console read that has begun a line return. First by cancelling it (Windows 10 on honours
    /// CancelIoEx for ReadConsole), which leaves nothing behind in the console; else, as when the reader is
    /// between two reads of the line and there is nothing to cancel, by typing the Enter it waits for.
    /// @return Whether the read was cancelled or the Enter typed.
    [[nodiscard]] bool endRead(ReaderState const& state)
    {
        if (::CancelIoEx(state.input, nullptr) != 0)
            return true;
        return injectEnter(state);
    }

    /// Ends the reader's line: back to Idle, unless the destructor has moved the phase to Stopped already,
    /// which it keeps.
    void endLine(ReaderState& state)
    {
        auto reading = ReaderPhase::Reading;
        std::ignore = state.phase.compare_exchange_strong(reading, ReaderPhase::Idle);
    }

    /// Reads one line from the console: waits on the console's input handle and on the wakeup, so that it
    /// can be stopped while the user has typed nothing. The Windows-only half of LineReader.
    void readLine(ReaderState& state)
    {
        // A line, echoed, with Ctrl+C its own: only in this mode does ReadConsoleW return at the Enter, with
        // the L'\n' that ends the line, whatever mode the console was left in. Restored as the read ends.
        auto previousMode = DWORD {};
        auto const hasMode = ::GetConsoleMode(state.input, &previousMode) != 0;
        if (hasMode)
            ::SetConsoleMode(state.input,
                             previousMode | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT);
        auto const restoreMode = core::Finally { [&state, hasMode, previousMode] {
            if (hasMode)
                ::SetConsoleMode(state.input, previousMode);
        } };
        auto const handles = std::array<HANDLE, 2> { state.input, state.wakeup.nativeHandle() };
        while (true)
        {
            if (::WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, INFINITE)
                != WAIT_OBJECT_0)
                return; // stopped (the wakeup is the second handle), or the wait failed
            // The console signals for every event, not only for typing: a focus change, a mouse move or a
            // resize is read away unseen, and only a key being pressed is a reason to read a line.
            auto record = INPUT_RECORD {};
            auto count = DWORD {};
            if (::PeekConsoleInputW(state.input, &record, 1, &count) == 0)
                return;
            if (count == 0)
                continue;
            if (record.EventType != KEY_EVENT || record.Event.KeyEvent.bKeyDown == 0)
            {
                if (::ReadConsoleInputW(state.input, &record, 1, &count) == 0)
                    return;
                continue;
            }
            // Typing began: the line is read to its Enter, which the user ends on their own time -- or which
            // the LineReader's destructor types for them (injectEnter()), to stop the read.
            // Begun only from Idle: if the destructor stopped the reader between the wait and now, it saw no
            // line begun and will type no Enter, so none may be begun for it either.
            auto idle = ReaderPhase::Idle;
            if (!state.phase.compare_exchange_strong(idle, ReaderPhase::Reading))
                return;
            auto text = std::wstring {};
            auto wide = std::array<wchar_t, MaxLine> {};
            while (true)
            {
                auto read = DWORD {};
                if (::ReadConsoleW(state.input, wide.data(), static_cast<DWORD>(wide.size()), &read, nullptr)
                        == 0
                    || read == 0)
                {
                    endLine(state);
                    return;
                }
                // Only what fits is kept, but the line is read to its end, so that no tail is left for
                // whatever reads the console next.
                text.append(wide.data(),
                            std::min<std::size_t>(read, MaxLine - std::min(text.size(), MaxLine)));
                if (wide[read - 1] == L'\n')
                    break;
            }
            endLine(state);
            auto line = std::string(MaxLine * 4, '\0');
            auto const bytes = ::WideCharToMultiByte(CP_UTF8,
                                                     0,
                                                     text.data(),
                                                     static_cast<int>(text.size()),
                                                     line.data(),
                                                     static_cast<int>(line.size()),
                                                     nullptr,
                                                     nullptr);
            line.resize(bytes > 0 ? static_cast<std::size_t>(bytes) : 0);
            state.sender->give(saysYes(line) ? vthost::PinDecision::Trust : vthost::PinDecision::Decline);
            return;
        }
    }
#else
    /// Reads one line from a descriptor: waits in poll() on it and on the wakeup, then reads raw, with no
    /// stdio lock that a process exiting would wait for. The POSIX half of LineReader.
    void readLine(ReaderState& state)
    {
        auto line = std::string {};
        auto buffer = std::array<char, 64> {};
        while (true)
        {
            auto fds =
                std::array { pollfd { .fd = state.input, .events = POLLIN, .revents = 0 },
                             pollfd { .fd = state.wakeup.nativeHandle(), .events = POLLIN, .revents = 0 } };
            if (::poll(fds.data(), fds.size(), -1) < 0)
            {
                if (errno == EINTR)
                    continue;
                return;
            }
            if (fds[1].revents != 0)
                return; // stopped: the answer is wanted no more
            auto const count = ::read(state.input, buffer.data(), buffer.size());
            if (count < 0 && errno == EINTR)
                continue;
            if (count > 0)
                line.append(buffer.data(), static_cast<std::size_t>(count));
            auto const ended = count <= 0;
            if (ended && line.empty())
                return; // the input ended with nothing typed: nobody trusted anything
            if (auto const end = line.find('\n'); ended || end != std::string::npos || line.size() > MaxLine)
            {
                state.sender->give(saysYes(line.substr(0, end)) ? vthost::PinDecision::Trust
                                                                : vthost::PinDecision::Decline);
                return;
            }
        }
    }
#endif

    /// Reads one line from the terminal on a thread of its own, until it has one, the input ends, or it is
    /// told to stop; stopping it is a signal and a join (on Windows, a typed Enter first), so nothing is left
    /// behind in a read.
    class LineReader final
    {
      public:
        /// @param input The terminal's input; borrowed.
        /// @param sender Told what the user decided, if they did.
        LineReader(core::platform::NativeHandle input, std::shared_ptr<vthost::PinAnswer::Sender> sender):
            _state { std::make_shared<ReaderState>(input, std::move(sender)) }, _thread { [state = _state] {
                readLine(*state);
                // No answer, if none was given: the last sender going is what ends the wait in Decline.
                state->sender.reset();
            } }
        {
        }

        /// Stops the thread and waits for it. On Windows a read that began a line ends only when cancelled
        /// or with an Enter, which is done for it here if the reader is still in it; the thread is then
        /// joined, on every platform, unless neither reached the read.
        ~LineReader()
        {
            _state->wakeup.signal();
#ifdef _WIN32
            // The read is ended only by the one that moved the phase out of Reading: a reader that has ended
            // its line already left it Idle, and an Enter typed then would go to the shell that is next.
            if (_state->phase.exchange(ReaderPhase::Stopped) == ReaderPhase::Reading && !endRead(*_state))
            {
                // Joining would wait for the user to press Enter. The thread is let go instead: it holds what
                // it uses, and an answer it reads after all reaches an answer nobody waits for any more.
                _thread.detach();
                return;
            }
#endif
            _thread.join();
        }

        LineReader(LineReader const&) = delete;
        LineReader& operator=(LineReader const&) = delete;
        LineReader(LineReader&&) = delete;
        LineReader& operator=(LineReader&&) = delete;

      private:
        std::shared_ptr<ReaderState> _state;
        std::thread _thread; ///< Declared last: it runs as soon as it is made, and uses the rest.
    };
} // namespace

core::async::Task<vthost::PinDecision> TerminalPinPrompt::confirm(std::string hostPort,
                                                                  std::string fingerprint)
{
    // The synchronous part of a task on a loop: where the answer's waiter is resumed.
    auto* const executor = core::async::currentExecutor();
    if (executor == nullptr)
        co_return vthost::PinDecision::Decline;

    _out << std::format("The daemon at {} is not known yet. It presents:\n"
                        "  {}\n"
                        "Compare with `contour daemon status` on that machine.\n"
                        "Trust this daemon? [y/N] ",
                        hostPort,
                        fingerprint)
         << std::flush;

    // The read waits for the user, so it is made on a thread of the prompt's own: the loop goes on serving
    // its timers and its stop meanwhile. The reader belongs to this wait: when the wait is cancelled, its
    // frame unwinds and the reader is woken and joined, so no read outlives it.
    auto const answer = std::make_shared<vthost::PinAnswer>(*executor);
    auto const reader = LineReader { _input, vthost::PinAnswer::senderOf(answer) };
    co_return co_await answer->await();
}

std::shared_ptr<vthost::IPinPrompt> makeTerminalPinPrompt(TerminalAttendance attendance,
                                                          core::platform::NativeHandle input,
                                                          std::ostream& out)
{
    if (attendance == TerminalAttendance::Unattended)
        return nullptr;
    return std::make_shared<TerminalPinPrompt>(input, out);
}

} // namespace contour::cli
