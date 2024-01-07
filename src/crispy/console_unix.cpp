#include <crispy/console.h>

#include <optional>

#if defined(__APPLE__)
    #include <util.h>
#elif defined(__FreeBSD__)
    #include <libutil.h>
#else
    #include <pty.h>
#endif

namespace crispy
{

struct console::Impl
{
    int in;
    int out;
    sequence_handler sequenceHandler;
};

console& console::get()
{
    static auto instance = console(STDIN_FILENO, STDOUT_FILENO);
    return instance;
}

console::console(int in, int out): _impl(new Impl { in, out }, [](Impl* p) { delete p; })
{
}

std::optional<console::size> console::window_size() const noexcept
{
    auto ws = winsize {};
    if (ioctl(_impl->out, TIOCSWINSZ, &ws) == -1)
        return std::nullopt;

    return size {
        .cells = {
            .columns = columns(ws.ws_col),
            .lines = lines(ws.ws_row),
        },
        .pixels = {
            .width = width(ws.ws_xpixel),
            .height = height(ws.ws_ypixel),
        },
    };
}

void console::write(std::string_view text)
{
    if (_impl->out < 0)
        return;

    (void) ::write(_impl->out, text.data(), text.size());
}

void console::set_sequence_handler(sequence_handler handler)
{
    _impl->sequenceHandler = std::move(handler);
}

void console::reset_sequence_handler()
{
    _impl->sequenceHandler = {};
}

} // namespace crispy
