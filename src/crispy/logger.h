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
#pragma once

#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <functional>
#include <fmt/format.h>

#if defined(__has_include)
    #if __has_include(<version>)
        #include <version>
    #endif
    #if __has_include(<source_location>)
        #include <source_location>
        #define CRISPY_SOURCE_LOCATION 1
        #define CRISPY_SOURCE_LOCATION_CURRENT() crispy::source_location::current()
        namespace crispy { using source_location = std::source_location; }
    #elif __has_include(<experimental/source_location>)
        #include <experimental/source_location>
        #define CRISPY_SOURCE_LOCATION 1
        #define CRISPY_SOURCE_LOCATION_CURRENT() crispy::source_location::current()
        namespace crispy { using source_location = std::experimental::source_location; }
    #else
        // Provide dummy impl for source_location below.
    #endif
#endif

namespace crispy {

namespace detail {
    class dummy_source_location {
      public:
        dummy_source_location(std::string_view _filename, int _line, std::string_view _functionName) :
            fileName_{ _filename },
            line_{ _line },
            functionName_{ _functionName }
        {}

        char const* file_name() const noexcept { return fileName_.c_str(); }
        int line() const noexcept { return line_; }
        char const* function_name() const noexcept { return functionName_.c_str(); }

      private:
        std::string fileName_;
        int line_;
        std::string functionName_;
    };
}

#if !defined(CRISPY_SOURCE_LOCATION)
    using source_location = detail::dummy_source_location;
#endif

class log_message {
  public:
    using Flush = std::function<void(log_message const&)>;

    log_message(Flush _flush, source_location _sloc) :
        flush_{ std::move(_flush) },
        location_{ std::move(_sloc) }
    {}

    ~log_message()
    {
        flush_(*this);
    }

    template <typename... Args>
    void write(std::string_view _message)
    {
        text_.append(_message);
    }

    template <typename... Args>
    void write(std::string_view _format, Args&&... _args)
    {
        text_.append(fmt::format(_format, std::forward<Args>(_args)...));
    }

    source_location const& location() const noexcept { return location_; }
    std::string const& text() const noexcept { return text_; }

  private:
    Flush flush_;
    source_location const location_;
    std::string text_;
};

class logging_sink {
  public:
    using Transform = std::function<std::string(log_message const&)>;
    using Writer = std::function<void(std::string_view const&)>;

    logging_sink(bool _enabled, Writer _writer, Transform _transform) :
        enabled_{ _enabled },
        transform_{ std::move(_transform) },
        writer_{ std::move(_writer) }
    {}

    logging_sink(bool _enabled, Writer _writer) :
        enabled_{ _enabled },
        transform_{ [this](auto const& x) { return standard_transform(x); } },
        writer_{ std::move(_writer) }
    {}

    logging_sink(bool _enabled, std::ostream& _output) :
        logging_sink(
            _enabled,
            [out = &_output](std::string_view const& text) { *out << text; out->flush(); }
        )
    {}

    void set_transform(Transform _transform) { transform_ = std::move(_transform); }
    void set_writer(Writer _writer) { writer_ = std::move(_writer); }

    bool enabled() const noexcept { return enabled_; }
    void enable(bool _enabled) { enabled_ = _enabled; }
    void toggle() noexcept { enabled_ = !enabled_; }

    void write(log_message const& _message)
    {
        if (enabled())
            writer_(transform_(_message));
    }

    /// Retrieves reference to standard debug-logging sink.
    static inline logging_sink& for_debug()
    {
        static auto instance = logging_sink(false, std::cout);
        return instance;
    }

    std::string standard_transform(log_message const& _message)
    {
        return fmt::format("[{}:{}] {}: {}\n",
            _message.location().file_name(),
            _message.location().line(),
            _message.location().function_name(),
            _message.text()
        );
    }

  private:
    bool enabled_;
    Transform transform_;
    Writer writer_;
};

}

#if defined(CRISPY_SOURCE_LOCATION)
    // XXX: sadly, this must be a global function so we can provide the fallback below.
    // TODO: Change that as soon as we get C++20's std::source_location on all major platforms supported.
    inline ::crispy::log_message debuglog(crispy::source_location _sloc = crispy::source_location::current())
    {
        return crispy::log_message([](::crispy::log_message const& m) { ::crispy::logging_sink::for_debug().write(m); }, _sloc);
    }
#elif defined(__GNUC__) || defined(__clang__)
    #define debuglog() (::crispy::log_message([](::crispy::log_message const& m) { ::crispy::logging_sink::for_debug().write(m); }, ::crispy::detail::dummy_source_location(__FILE__, __LINE__, __FUNCTION__)))
#elif defined(__FUNCSIG__)
    #define debuglog() (::crispy::log_message([](::crispy::log_message const& m) { ::crispy::logging_sink::for_debug().write(m); }, ::crispy::detail::dummy_source_location(__FILE__, __LINE__, __FUNCSIG__)))
#endif

