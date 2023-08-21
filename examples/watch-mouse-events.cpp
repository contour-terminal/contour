// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/InputGenerator.h>
#include <vtbackend/Sequence.h>
#include <vtbackend/primitives.h>

#include <vtparser/Parser.h>
#include <vtparser/ParserEvents.h>

#include <vtpty/UnixUtils.h>

#include <fmt/format.h>

#include <csignal>
#include <iostream>

#if defined(__APPLE__)
    #include <util.h>
#else
    #include <termios.h>
#endif

#include <fcntl.h>
#include <unistd.h>

using namespace std;

namespace
{

using namespace terminal;

struct BasicParserEvents: public NullParserEvents // {{{
{
    Sequence _sequence {};
    SequenceParameterBuilder _parameterBuilder;

    BasicParserEvents(): _parameterBuilder(_sequence.parameters()) {}

    void collect(char ch) override { _sequence.intermediateCharacters().push_back(ch); }

    void collectLeader(char leader) noexcept override { _sequence.setLeader(leader); }

    void clear() noexcept override
    {
        _sequence.clearExceptParameters();
        _parameterBuilder.reset();
    }

    void paramDigit(char ch) noexcept override
    {
        _parameterBuilder.multiplyBy10AndAdd(static_cast<uint8_t>(ch - '0'));
    }

    void paramSeparator() noexcept override { _parameterBuilder.nextParameter(); }

    void paramSubSeparator() noexcept override { _parameterBuilder.nextSubParameter(); }

    void param(char ch) noexcept override
    {
        switch (ch)
        {
            case ';': paramSeparator(); break;
            case ':': paramSubSeparator(); break;
            case '0':
            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
            case '7':
            case '8':
            case '9': paramDigit(ch); break;
        }
    }

    virtual void handleSequence() = 0;

    void executeSequenceHandler()
    {
        _parameterBuilder.fixiate();
        handleSequence();
    }

    void dispatchESC(char finalChar) override
    {
        _sequence.setCategory(FunctionCategory::ESC);
        _sequence.setFinalChar(finalChar);
        executeSequenceHandler();
    }

    void dispatchCSI(char finalChar) override
    {
        _sequence.setCategory(FunctionCategory::CSI);
        _sequence.setFinalChar(finalChar);
        executeSequenceHandler();
    }

    void startOSC() override { _sequence.setCategory(FunctionCategory::OSC); }

    void putOSC(char ch) override
    {
        if (_sequence.intermediateCharacters().size() + 1 < Sequence::MaxOscLength)
            _sequence.intermediateCharacters().push_back(ch);
    }

    void dispatchOSC() override
    {
        auto const [code, skipCount] = parser::extractCodePrefix(_sequence.intermediateCharacters());
        _parameterBuilder.set(static_cast<Sequence::Parameter>(code));
        _sequence.intermediateCharacters().erase(0, skipCount);
        executeSequenceHandler();
        clear();
    }

    void hook(char finalChar) override
    {
        _sequence.setCategory(FunctionCategory::DCS);
        _sequence.setFinalChar(finalChar);
        executeSequenceHandler();
    }
};
// }}}

struct MouseTracker final: public BasicParserEvents
{
    static bool _running;

    int mouseButton = -1;
    int line = -1;
    int column = -1;
    bool uiHandledHint = false;
    termios savedTermios;

    parser::Parser<ParserEvents> vtInputParser;

    MouseTracker() noexcept: savedTermios { detail::getTerminalSettings(STDIN_FILENO) }, vtInputParser { *this }
    {
        auto tio = savedTermios;
        tio.c_lflag &= static_cast<tcflag_t>(~(ECHO | ICANON));
        tio.c_cc[VMIN] = 1;  // Report as soon as 1 character is available.
        tio.c_cc[VTIME] = 0; // Disable timeout (no need).
        detail::applyTerminalSettings(STDIN_FILENO, tio);

        writeToTTY("\033[?2029h"); // enable passive mouse reporting
        writeToTTY("\033[?2030h"); // enable text selection reporting
        writeToTTY("\033[?1003h"); // enable tracking any mouse event
        writeToTTY("\033[?25l");   // hide text cursor

        signal(SIGINT, signalHandler);
        signal(SIGTERM, signalHandler);
        signal(SIGSTOP, signalHandler);
    }

    ~MouseTracker() override
    {
        detail::applyTerminalSettings(STDIN_FILENO, savedTermios);
        writeToTTY("\033[?2029l"); // disable passive mouse reporting
        writeToTTY("\033[?2030l"); // disable text selection reporting
        writeToTTY("\033[?25h");   // show text cursor
        writeToTTY("\nTerminating\n");
    }

    static void signalHandler(int signo)
    {
        _running = false;
        signal(signo, SIG_DFL);
    }

    void execute(char /*controlCode*/) override { _running = false; }

    void print(char32_t ch) override
    {
        if (ch == U'q' || ch == U'Q')
            _running = false;
    }

    size_t print(std::string_view text, size_t /*columnsUsed*/) override
    {
        if (text == "q" || text == "Q")
            _running = false;
        return 0;
    }

    int run()
    {
        checkPassiveMouseTrackingSupport();
        while (_running)
        {
            writeToTTY(fmt::format("\rMouse position {}:{}, 0x{:X}, {} ({})\033[K",
                                   line,
                                   column,
                                   mouseButton,
                                   uiHandledHint ? "UI handled" : "idle",
                                   selectionStateString()));
            processInput();
        }
        writeToTTY("\n"sv);
        return EXIT_SUCCESS;
    }

    string selectionStateString()
    {
        if (selection.mode == 0)
            return "no text selection";
        auto const mode = [&]() {
            switch (selection.mode)
            {
                case 1: return "Linear";
                case 2: return "Full Line";
                case 3: return "Rectangular";
                default: return "Unknown";
            }
        }();

        return fmt::format("{}; {} .. {}", mode, selection.from, selection.to);
    }

    void checkPassiveMouseTrackingSupport()
    {
        writeToTTY("\033[?2029$p");
        while (!_decrpm)
            processInput();

        auto const state = _decrpm.value().second;
        auto const supported = state == 1 || state == 2;
        fmt::print("Passive mouse tracking: {}\n", supported ? "supported" : "not supported");
    }

    void writeToTTY(string_view s) noexcept { ::write(STDOUT_FILENO, s.data(), s.size()); }

    void processInput()
    {
        char buf[128];
        auto n = ::read(STDIN_FILENO, buf, sizeof(buf));
        if (n > 0)
            vtInputParser.parseFragment(string_view(buf, static_cast<size_t>(n)));
    }

    void handleSequence() override
    {
        if (_sequence.leaderSymbol() == '<' && _sequence.finalChar() == 'M')
        {
            // CSI < {ButtonStates} ; {COLUMN} ; {LINE} ; {uiHandledHint} M
            // NB: button states on param index 0
            mouseButton = _sequence.param_or(0, 0);
            column = _sequence.param_or(1, -2);
            line = _sequence.param_or(2, -2);
            uiHandledHint = _sequence.param_or(3, 0) != 0;
        }
        else if (_sequence.leaderSymbol() == '?' && _sequence.intermediateCharacters() == "$"
                 && _sequence.finalChar() == 'y' && _sequence.parameterCount() == 2)
        {
            // Receive now something like this: CSI ? 2029 ; 0 $ y
            _decrpm = pair { _sequence.param(0), _sequence.param(1) };
        }
        else if (_sequence.leaderSymbol() == '>' && _sequence.finalChar() == 'M')
        {
            // CSI > M
            // CSI > {SelectionMode} ; {StartLine} ; {StartColumn} ; {EndLine} ; EndColumn M
            if (_sequence.parameterCount() == 0)
            {
                selection.mode = 0;
                selection.from = {};
                selection.to = {};
            }
            else if (_sequence.parameterCount() == 5)
            {
                selection.mode = _sequence.param(0);
                selection.from.line = _sequence.param<LineOffset>(1);
                selection.from.column = _sequence.param<ColumnOffset>(2);
                selection.to.line = _sequence.param<LineOffset>(3);
                selection.to.column = _sequence.param<ColumnOffset>(4);
            }
        }
        _sequence.clear();
    }

    struct
    {
        unsigned mode {};
        CellLocation from {};
        CellLocation to {};
    } selection;
    std::optional<std::pair<int, int>> _decrpm;
};

bool MouseTracker::_running = true;

} // namespace

int main()
{
    auto mouseTracker = MouseTracker {};
    return mouseTracker.run();
}
