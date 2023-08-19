// SPDX-License-Identifier: Apache-2.0
#include <crispy/logstore.h>

namespace logstore
{

sink::sink(bool enabled, writer wr): _enabled { enabled }, _writer { std::move(wr) }
{
}

sink::sink(bool enabled, std::ostream& output):
    sink(enabled, [out = &output](std::string_view text) {
        *out << text;
        out->flush();
    })
{
}

sink::sink(bool enabled, std::shared_ptr<std::ostream> f):
    sink(enabled, [f = std::move(f)](std::string_view text) {
        *f << text;
        f->flush();
    })
{
}

sink& sink::console()
{
    static auto instance = sink(false, std::cout);
    return instance;
}

sink& sink::error_console() // NOLINT(readability-identifier-naming)
{
    static auto instance = sink(true, std::cerr);
    return instance;
}

} // namespace logstore
