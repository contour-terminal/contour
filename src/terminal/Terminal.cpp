#include <terminal/Terminal.h>

#include <terminal/Generator.h>
#include <terminal/Util.h>

using namespace std;

namespace terminal {

Terminal::Terminal(size_t cols,
                   size_t rows,
                   Screen::Reply reply,
                   Logger logger,
                   Hook onCommands)
  : logger_{move(logger)},
    screen_{
        cols,
        rows,
        move(reply),
        [this](auto const& msg) { logger_(msg); },
        move(onCommands)
    }
{
}

void Terminal::write(char const* data, size_t size)
{
    screen_.write(data, size);
}

}  // namespace terminal
