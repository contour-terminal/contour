#include <terminal/Terminal.h>
#include <terminal/pty/MockPty.h>
#include <iostream>
#include <optional>
#include <fmt/format.h>
#include <random>

class HeadlessBench: public terminal::Terminal::Events
{
public:
    HeadlessBench(terminal::PageSize _pageSize,
                  int _ptyReadBufferSize,
                  std::optional<terminal::LineCount> _maxHistoryLineCount);

    void run();

private:
    std::unique_ptr<terminal::MockPty> pty_;
    terminal::Terminal vt_;
    bool running_ = false;
};

using namespace std;

HeadlessBench::HeadlessBench(terminal::PageSize _pageSize,
                             int _ptyReadBufferSize,
                             std::optional<terminal::LineCount> _maxHistoryLineCount):
    pty_{std::make_unique<terminal::MockPty>(_pageSize)},
    vt_{
        *pty_,
        _ptyReadBufferSize,
        *this,
        _maxHistoryLineCount
    }
{
    vt_.screen().setMode(terminal::DECMode::AutoWrap, true);
}

void HeadlessBench::run()
{
    running_ = true;

    std::string text;
    text.resize(8192);

    std::random_device rand_dev;
    std::mt19937 generator(rand_dev());
    std::uniform_int_distribution<char> distr(0x20, 0x7E);
    for (size_t i = 0; i < text.size(); ++i)
        text[i] = distr(generator);

    uint64_t i = 0;
    while (running_)
    {
        assert(pty_->stdoutBuffer().size() == 0);
        pty_->stdoutBuffer().append(text);

        vt_.processInputOnce();

        i++;
        if (i > 10000)
            running_ = false;
    }
}

int main(int argc, char const* argv[])
{
    auto pageSize = terminal::PageSize{terminal::LineCount(25), terminal::ColumnCount(80)};
    auto const ptyReadBufferSize = 8192;
    auto maxHistoryLineCount = optional{terminal::LineCount(10000)};
    auto hb = HeadlessBench{pageSize, ptyReadBufferSize, maxHistoryLineCount};

    hb.run();

    return EXIT_SUCCESS;
}
