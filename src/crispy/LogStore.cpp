// SPDX-License-Identifier: Apache-2.0
#include <crispy/LogStore.hpp>

namespace logstore
{

Sink::Sink(bool enabled, Writer wr): _enabled { enabled }, _writer { std::move(wr) }
{
}

Sink::Sink(bool enabled, std::ostream& output):
    Sink(enabled, [out = &output](std::string_view text) {
        *out << text;
        out->flush();
    })
{
}

Sink::Sink(bool enabled, std::shared_ptr<std::ostream> f):
    Sink(enabled, [f = std::move(f)](std::string_view text) {
        *f << text;
        f->flush();
    })
{
}

Sink& Sink::console()
{
    static auto instance = Sink(false, std::cout);
    return instance;
}

Sink& Sink::errorConsole()
{
    static auto instance = Sink(true, std::cerr);
    return instance;
}

} // namespace logstore
