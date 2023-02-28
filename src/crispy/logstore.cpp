/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <crispy/logstore.h>

namespace logstore
{

Sink::Sink(bool enabled, Writer writer): _enabled { enabled }, _writer { std::move(writer) }
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

Sink& Sink::error_console() // NOLINT(readability-identifier-naming)
{
    static auto instance = Sink(true, std::cerr);
    return instance;
}

} // namespace logstore
