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

#include <crispy/indexed.h>
#include <crispy/algorithm.h>

#include <cassert>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <functional>
#include <vector>

#include <fmt/format.h>

#if defined(__has_include)
    #if __has_include(<version>)
        #include <version>
    #endif
    // XXX GCC 11 seems to break here because it states that <source_location> is present
    //     and indeed it can be included, but it's not exposing std::source_location.
    // #if __has_include(<source_location>)
    //     #include <source_location>
    //     #define CRISPY_SOURCE_LOCATION 1
    //     #define CRISPY_SOURCE_LOCATION_CURRENT() crispy::source_location::current()
    //     namespace crispy { using source_location = std::source_location; }
    #if __has_include(<experimental/source_location>)
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

namespace debugtag
{
    struct tag_info {
        std::string name;
        bool enabled;
        std::string description;
    };
    struct tag_id { size_t value; };

    inline std::vector<tag_info>& store()
    {
        static std::vector<tag_info> tagStore;
        return tagStore;
    }

    // inline tag_id get(std::string_view _name)
    // {
    //     for (auto const && [i, t] : crispy::indexed(store()))
    //         if (_name == t.name)
    //             return tag_id{ i };
    //
    //     store().emplace_back(std::pair{_name, false});
    //     return tag_id{ store().size() - 1 };
    // }

    inline tag_info const& get(tag_id _id)
    {
        return store().at(_id.value);
    }

    inline tag_id make(std::string_view _name, std::string_view _description, bool _enabled = false)
    {
        assert(crispy::none_of(store(), [&](tag_info const& x) { return x.name == _name; }));
        store().emplace_back(tag_info{std::string(_name), _enabled, std::string(_description)});
        return tag_id{ store().size() - 1 };
    }

    inline void enable(tag_id _tag)
    {
        store().at(_tag.value).enabled = true;
    }

    inline void disable(tag_id _tag)
    {
        store().at(_tag.value).enabled = false;
    }

    inline bool enabled(tag_id _tag) noexcept
    {
        return _tag.value < store().size() && store().at(_tag.value).enabled;
    }
}

class log_message {
  public:
    using Flush = std::function<void(log_message const&)>;

    log_message(Flush _flush, source_location _sloc, debugtag::tag_id _tag) :
        flush_{ std::move(_flush) },
        location_{ std::move(_sloc) },
        tag_{ _tag }
    {}

    ~log_message()
    {
        flush_(*this);
    }

    template <typename... Args>
    void write(std::string_view _message)
    {
        if (debugtag::enabled(tag_))
            text_.append(_message);
    }

    template <typename... Args>
    void write(std::string_view _format, Args&&... _args)
    {
        if (debugtag::enabled(tag_))
            text_.append(fmt::format(_format, std::forward<Args>(_args)...));
    }

    source_location const& location() const noexcept { return location_; }
    debugtag::tag_id tag() const noexcept { return tag_; }
    std::string const& text() const noexcept { return text_; }

  private:
    Flush flush_;
    source_location const location_;
    debugtag::tag_id const tag_;
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
        return fmt::format("[{}:{}:{}]: {}\n",
            debugtag::get(_message.tag()).name,
            _message.location().file_name(),
            _message.location().line(),
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
    inline ::crispy::log_message debuglog(::crispy::debugtag::tag_id _tag, crispy::source_location _sloc = crispy::source_location::current())
    {
        return crispy::log_message([](::crispy::log_message const& m) { ::crispy::logging_sink::for_debug().write(m); }, _sloc, _tag);
    }
#elif defined(__GNUC__) || defined(__clang__)
    #define debuglog(_tag) (::crispy::log_message([](::crispy::log_message const& m) { ::crispy::logging_sink::for_debug().write(m); }, ::crispy::detail::dummy_source_location(__FILE__, __LINE__, __FUNCTION__), (_tag)))
#elif defined(__func__)
    #define debuglog(_tag) (::crispy::log_message([](::crispy::log_message const& m) { ::crispy::logging_sink::for_debug().write(m); }, ::crispy::detail::dummy_source_location(__FILE__, __LINE__, __func__), (_tag)))
#elif defined(__FUNCTION__)
    #define debuglog(_tag) (::crispy::log_message([](::crispy::log_message const& m) { ::crispy::logging_sink::for_debug().write(m); }, ::crispy::detail::dummy_source_location(__FILE__, __LINE__, __FUNCTION__), (_tag)))
#endif

