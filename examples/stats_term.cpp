#include <terminal/Generator.h>
#include <terminal/OutputHandler.h>
#include <terminal/Parser.h>
#include <terminal/Process.h>
#include <terminal/Screen.h>
#include <terminal/Terminal.h>
#include <terminal/Util.h>

#include <fmt/format.h>

#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <numeric>
#include <optional>
#include <thread>

#include <cstddef>

#include <fcntl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

using std::placeholders::_1;
using fmt::format;

using namespace std;

class NonBlocking {
  public:
    explicit NonBlocking(int fd) : fd_{fd} { set(true); }
    ~NonBlocking() { set(false); }

  private:
    void set(bool enable)
    {
        if (int flags = fcntl(fd_, F_GETFL); flags != -1)
        {
            if (enable)
                flags |= O_NONBLOCK;
            else
                flags &= ~O_NONBLOCK;
            fcntl(fd_, F_SETFL, flags);
        }
    }

    int fd_;
};

enum class Mode {
    PassThrough,
    Proxy,
    Redraw,
};

class Forwarder {
  public:
    explicit Forwarder(Mode mode,
                       terminal::WindowSize const& windowSize,
                       string shell = terminal::Process::loginShell())
        : mode_{mode},
          tio_{setupTerminalSettings(STDIN_FILENO)},
          logger_{ofstream{"trace.log", ios::trunc}},
          process_{windowSize, shell},
          terminal_{
              windowSize.columns,
              windowSize.rows,
              bind(&Forwarder::screenReply, this, _1),
              [this](auto const& msg) { log("terminal: {}", msg); },
              bind(&Forwarder::onStdout, this, _1)
          }
    {
        // TODO: when outside term changes windows size, propagate it into here too.
        // TODO: query current cursor position and initialize cursor in internal screen to it OR reset outside screen, too
        log("Forwarder-Mode: {}", static_cast<int>(mode_));
    }

    void onStdout(vector<terminal::Command> const& commands)
    {
        auto const generated = terminal::Generator::generate(commands);

        log("create: {}", terminal::escape(generated));
        for (terminal::Command const& command: commands)
            log("command: {}", to_string(command));

        switch (mode_)
        {
            case Mode::Proxy:
                write(generated);
                break;
            case Mode::Redraw:
                redraw();
                break;
            case Mode::PassThrough:
                break;
        }
    }

    // PoC-style naive implementation of a full screen redraw
    void redraw()
    {
        terminal::Generator generator{[this](char const* s, size_t n) { write(s, n); }};

        generator(terminal::SetMode{terminal::Mode::VisibleCursor, false});
        generator(terminal::SetMode{terminal::Mode::AutoWrap, false});
        generator(terminal::SetGraphicsRendition{terminal::GraphicsRendition::Reset});

        terminal_.screen().render(
            [&](auto row, auto col, terminal::Screen::Cell const& cell) {
                generator(terminal::MoveCursorTo{row, col});
                generator(terminal::SetForegroundColor{cell.attributes.foregroundColor});
                generator(terminal::SetBackgroundColor{cell.attributes.backgroundColor});

                // TODO: styles

                if (cell.character)
                    generator(terminal::AppendChar{cell.character});
                else
                    generator(terminal::AppendChar{' '}); // FIXME workaround to ensure it's drawn
            }
        );

        // position cursor
        generator(terminal::MoveCursorTo{terminal_.screen().currentRow(),
                                         terminal_.screen().currentColumn()});

        // (TODO: make visible ONLY if meant to be visible)
        generator(terminal::SetMode{terminal::Mode::VisibleCursor, true});
    }

    void write(char const* data, size_t size)
    {
        ::write(STDOUT_FILENO, data, size);
    }

    void write(string_view const& text)
    {
        write(text.data(), text.size());
    }

    template <typename... Args>
    void write(string_view const& text, Args&&... args)
    {
        write(fmt::format(text, forward<Args>(args)...));
    }

    ~Forwarder()
    {
        // restore some settings
        terminal::Generator generator{[this](char const* s, size_t n) { write(s, n); }};
        generator(terminal::SetMode{terminal::Mode::VisibleCursor, true});

        // restore flags upon exit
        tcsetattr(STDIN_FILENO, TCSANOW, &tio_);
    }

    int main()
    {
        while (runLoopOnce())
            ;

        return EXIT_SUCCESS;
    }

  private:
    void screenReply(string_view const& message)
    {
        (void) ::write(process_.masterFd(), message.data(), message.size());
    }

    bool runLoopOnce()
    {
        fd_set sin;
        fd_set sout;
        fd_set serr;

        FD_ZERO(&sin);
        FD_ZERO(&sout);
        FD_ZERO(&serr);

        FD_SET(STDIN_FILENO, &sin);
        FD_SET(process_.masterFd(), &sin);
        int const nfd = max(STDIN_FILENO, process_.masterFd()) + 1;

        select(nfd, &sin, &sout, &serr, nullptr);

        if (FD_ISSET(STDIN_FILENO, &sin))
        {
            auto const _ = NonBlocking{STDIN_FILENO};
            char buf[4096];
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n <= 0)
                return false;
            log("input: {}", terminal::escape(buf, next(buf, n)));
            if (process_.send(buf, n) != n)
                return false;
        }

        if (FD_ISSET(process_.masterFd(), &sin))
        {
            auto const _ = NonBlocking{process_.masterFd()};
            uint8_t buf[4096];
            if (ssize_t n = read(process_.masterFd(), buf, sizeof(buf)); n > 0)
            {
                log("output: {}", terminal::escape(buf, buf + n));
                terminal_.write((char const*) buf, n);
                if (mode_ == Mode::PassThrough)
                    write(string_view{(char const*)buf, static_cast<size_t>(n)});
            }
            else
                return false;
        }

        return true;
    }

    void log(string_view const& msg)
    {
        if (logger_)
            *logger_ << msg << endl;
    }

    template <typename... Args>
    void log(string_view const& msg, Args&&... args)
    {
        if (logger_)
            *logger_ << fmt::format(msg, forward<Args>(args)...) << endl;
    }

    static termios getTerminalSettings(int fd)
    {
        termios tio;
        tcgetattr(STDIN_FILENO, &tio);
        return tio;
    }

    static pair<termios, termios> constructTerminalSettings(int fd)
    {
        auto const save = getTerminalSettings(fd);
        auto tio = save;

        // input flags
        tio.c_iflag |= IGNBRK;    // Ignore Break condition on input.
        tio.c_iflag &= ~IXON;     // Disable CTRL-S / CTRL-Q on output.
        tio.c_iflag &= ~IXOFF;    // Disable CTRL-S / CTRL-Q on input.
        tio.c_iflag &= ~ICRNL;    // Ensure CR isn't translated to NL.
        tio.c_iflag &= ~INLCR;    // Ensure NL isn't translated to CR.
        tio.c_iflag &= ~IGNCR;    // Ensure CR isn't ignored.
        tio.c_iflag &= ~IMAXBEL;  // Ensure beeping on full input buffer isn't enabled.
        tio.c_iflag &= ~ISTRIP;   // Ensure stripping of 8th bit on input isn't enabled.

        // output flags
        tio.c_oflag &= ~OPOST;   // Don't enable implementation defined output processing.
        tio.c_oflag &= ~ONLCR;   // Don't map NL to CR-NL.
        tio.c_oflag &= ~OCRNL;   // Don't map CR to NL.
        tio.c_oflag &= ~ONLRET;  // Don't output CR.

        // control flags

        // local flags
        tio.c_lflag &= ~IEXTEN;  // Don't enable implementation defined input processing.
        tio.c_lflag &= ~ICANON;  // Don't enable line buffering (Canonical mode).
        tio.c_lflag &= ~ECHO;    // Don't echo input characters.
        tio.c_lflag &= ~ISIG;    // Don't generate signal upon receiving characters for
                                 // INTR, QUIT, SUSP, DSUSP.

        // special characters
        tio.c_cc[VMIN] = 1;   // Report as soon as 1 character is available.
        tio.c_cc[VTIME] = 0;  // Disable timeout (no need).

        return {tio, save};
    }

    static termios setupTerminalSettings(int fd)
    {
        auto const [tio, save] = constructTerminalSettings(fd);

        if (tcsetattr(STDIN_FILENO, TCSANOW, &tio) == 0)
            tcflush(STDIN_FILENO, TCIOFLUSH);

        return save;
    }

  private:
    Mode const mode_;
    termios tio_;
    optional<ofstream> logger_;
    terminal::Process process_;
    terminal::Terminal terminal_;
    string generated_;
};

int main(int argc, char const* argv[])
{
    auto windowSize = terminal::currentWindowSize();
    cout << "Host Window Size: " << windowSize.columns << "x" << windowSize.rows << endl;

    return Forwarder{Mode::Redraw, windowSize}.main();
}