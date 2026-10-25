// SPDX-License-Identifier: Apache-2.0
#include <core/log/LogStore.hpp>

#include <functional>
#include <iostream>

namespace core::log
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
    static auto instance = Sink(false, std::cerr);
    return instance;
}

Sink& Sink::errorConsole()
{
    static auto instance = Sink(true, std::cerr);
    return instance;
}

namespace
{
    /// What setSink() and setFormatter() last installed, which a category constructed later
    /// starts out with.
    struct Installed
    {
        std::reference_wrapper<Sink> sink;
        Category::Formatter formatter;
    };

    /// @return The store's installed sink and formatter.
    ///
    /// A function-local static, like the registry get() returns, so a category constructed during
    /// static initialisation -- errorLog is one -- finds it initialised whatever the order of
    /// translation units.
    [[nodiscard]] Installed& installed()
    {
        static auto instance = Installed { .sink = Sink::console(), .formatter = {} };
        return instance;
    }
} // namespace

void setSink(Sink& sink)
{
    installed().sink = sink;
    for (auto const& category: get())
        category.get().setSink(sink);
}

void setFormatter(Category::Formatter const& f)
{
    installed().formatter = f;
    for (auto const& category: get())
        category.get().setFormatter(f);
}

Sink& installedSink()
{
    return installed().sink.get();
}

Category::Formatter const& installedFormatter()
{
    return installed().formatter;
}

} // namespace core::log
