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

#include <algorithm>
#include <functional>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>
#include <cassert>

#include <fmt/format.h>

#if __has_include(<source_location>)
    #include <source_location>
#elif __has_include(<experimental/source_location>)
    #include <experimental/source_location>
    #define LOGSTORE_HAS_EXPERIMENTAL_SOURCE_LOCATION 1
#endif

namespace logstore
{

class Category;
class Sink;

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

#if defined(__cpp_lib_source_location)
    using source_location = std::source_location;
#elif defined(LOGSTORE_HAS_EXPERIMENTAL_SOURCE_LOCATION)
    using source_location = std::experimental::source_location;
#else
    using source_location = detail::dummy_source_location;
#endif

#if defined(__cpp_lib_source_location) || defined(LOGSTORE_HAS_EXPERIMENTAL_SOURCE_LOCATION)
    #define LOGSTORE_THIS() (::logstore::source_location::current())
#elif defined(__GNUC__) || defined(__clang__)
    #define LOGSTORE_THIS() (::logstore::detail::dummy_source_location(__FILE__, __LINE__, __FUNCTION__))
#elif defined(__func__)
    #define LOGSTORE_THIS() (::logstore::detail::dummy_source_location(__FILE__, __LINE__, __func__))
#elif defined(__FUNCTION__)
    #define LOGSTORE_THIS() (::logstore::detail::dummy_source_location(__FILE__, __LINE__, __FUNCTION__))
#endif

#define LOGSTORE(x) (x(LOGSTORE_THIS()))

class MessageBuilder
{
private:
    Category const& _category;
    source_location _location;
    std::string _buffer;

public:
    MessageBuilder(Category const& cat, source_location loc);

    Category const& category() const noexcept { return _category; }
    source_location const& location() const noexcept { return _location; }

    std::string const& text() const noexcept { return _buffer; }

    MessageBuilder& append(std::string_view msg)
    {
        _buffer += msg;
        return *this;
    }

    template <typename... Args>
    MessageBuilder& append(std::string_view msg, Args... args)
    {
        _buffer += fmt::format(msg, std::forward<Args>(args)...);
        return *this;
    }

    MessageBuilder& operator()(std::string_view msg)
    {
        return append(msg);
    }

    template <typename... Args>
    MessageBuilder& operator()(std::string_view msg, Args... args)
    {
        return append(msg, std::forward<Args>(args)...);
    }

    std::string message() const;

    ~MessageBuilder();
};

/// Defines a logging Category, such as: error, warning, metrics, vt.backend, or renderer.
///
/// A program can have multiple logging categories, all pointing to the same
/// or each to an individual logging sink.
class Category
{
public:
    using Formatter = std::function<std::string(MessageBuilder const&)>;
    enum class State { Enabled, Disabled };
    enum class Visibility { Public, Hidden };

    Category(std::string_view name,
             std::string_view desc,
             State state = State::Disabled,
             Visibility visibility = Visibility::Public) noexcept;
    ~Category();

    constexpr std::string_view name() const noexcept { return _name; }
    constexpr std::string_view description() const noexcept { return _description; }

    constexpr bool is_enabled() const noexcept { return _state == State::Enabled; }
    constexpr void enable(bool enabled = true) noexcept { _state = enabled ? State::Enabled : State::Disabled; }
    constexpr void disable() noexcept { _state = State::Disabled; }

    constexpr bool visible() const noexcept { return _visibility == Visibility::Public; }
    constexpr void set_visible(bool visible) { _visibility = visible ? Visibility::Public : Visibility::Hidden; }

    constexpr operator bool () const noexcept { return is_enabled(); }

    Formatter const& formatter() const { return _formatter; }
    void set_formatter(Formatter formatter) { _formatter = std::move(formatter); }

    void set_sink(logstore::Sink& s) { _sink = s; }
    logstore::Sink& sink() const noexcept { return _sink.get(); }

    MessageBuilder write(source_location location) const { return MessageBuilder(*this, std::move(location)); }
    MessageBuilder operator()(source_location location) const { return MessageBuilder(*this, std::move(location)); }

    static std::string default_formatter(MessageBuilder const& _message);

private:
    std::string_view _name;
    std::string_view _description;
    State _state;
    Visibility _visibility;
    Formatter _formatter;
    std::reference_wrapper<logstore::Sink> _sink;
};

/// Logging Sink API.
///
/// Such as the console, a log file, or UDP endpoint.
class Sink
{
public:
    using Writer = std::function<void(std::string_view const&)>;

    Sink(bool _enabled, Writer _writer) :
        writer_{ std::move(_writer) }
    {}

    Sink(bool _enabled, std::ostream& _output) :
        Sink(
            _enabled,
            [out = &_output](std::string_view text) { *out << text; out->flush(); }
        )
    {}

    void set_writer(Writer _writer);

    /// Writes given built message to this sink.
    void write(MessageBuilder const& _message);

    /// Retrieves reference to standard debug-logging sink.
    static inline Sink& console()
    {
        static auto instance = Sink(false, std::cout);
        return instance;
    }

    static inline Sink& error_console()
    {
        static auto instance = Sink(false, std::cerr);
        return instance;
    }

  private:
    Writer writer_;
};

std::vector<std::reference_wrapper<Category>>& get();
Category* get(std::string_view categoryName);
void set_sink(Sink& _sink);
void set_formatter(Category::Formatter const& f);
void enable(std::string_view categoryName, bool enabled = true);
void disable(std::string_view categoryName);

// {{{ implementation
inline std::string MessageBuilder::message() const
{
    if (_category.formatter())
        return _category.formatter()(*this);
    else if (!_buffer.empty() && _buffer.back() == '\n')
        return _buffer;
    else if (!_buffer.empty())
        return _buffer + '\n';
    else
        return "";
}

inline std::vector<std::reference_wrapper<Category>>& get()
{
    static std::vector<std::reference_wrapper<Category>> logStore;
    return logStore;
}

inline Category* get(std::string_view categoryName)
{
    for (auto const& cat: get())
        if (cat.get().name() == categoryName)
            return &cat.get();
    return nullptr;
}

inline void set_sink(Sink& s)
{
    for (auto const& cat: get())
        cat.get().set_sink(s);
}

inline void set_formatter(Category::Formatter const& f)
{
    for (auto const& cat: get())
        cat.get().set_formatter(f);
}

inline void enable(std::string_view categoryName, bool enabled)
{
    for (auto const& cat: get())
        if (cat.get().name() == categoryName)
            cat.get().enable(enabled);
}

inline void disable(std::string_view categoryName)
{
    enable(categoryName, false);
}

inline MessageBuilder::MessageBuilder(logstore::Category const& cat, source_location loc):
    _category{cat},
    _location{std::move(loc)}
{
}

inline MessageBuilder::~MessageBuilder()
{
    _category.sink().write(*this);
}

inline Category::Category(std::string_view name,
                          std::string_view desc,
                          State state,
                          Visibility visibility) noexcept:
    _name{name},
    _description{desc},
    _state{state},
    _visibility{visibility},
    _sink{logstore::Sink::console()}
{
    assert(std::none_of(get().begin(), get().end(), [&](Category const& x) { return x.name() == _name; }));
    get().emplace_back(*this);
}

inline Category::~Category()
{
    for (auto i = get().begin(), e = get().end(); i != e; ++i)
    {
        if (&i->get() == this)
        {
            get().erase(i);
            break;
        }
    }
}

inline std::string Category::default_formatter(MessageBuilder const& _message)
{
    return fmt::format("[{}:{}:{}]: {}\n",
        _message.category().name(),
        _message.location().file_name(),
        _message.location().line(),
        _message.text()
    );
}


inline void Sink::write(MessageBuilder const& _message)
{
    if (_message.category().is_enabled())
        writer_(_message.message());
}

inline void Sink::set_writer(Writer _writer)
{
    writer_ = std::move(_writer);
}
// }}}

auto inline ErrorLog = logstore::Category("error", "Error Logger", Category::State::Enabled);

#define errorlog() (LOGSTORE(::logstore::ErrorLog))

}
