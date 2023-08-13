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

#include <crispy/algorithm.h>
#include <crispy/utils.h>

#include <fmt/format.h>

#include <algorithm>
#include <cassert>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#if defined __has_include
    #if __cpp_lib_source_location
        #include <source_location>
    #endif
#endif

// NB: Don't do that now. It seems to only cause problems, such as
// __has_include reports presense and in can in fact be included, but it's
// not giving us the expected std::...source_location, wow.
//
// #if __has_include(<source_location>) && !defined(_WIN32)
//     #include <source_location>
// #elif __has_include(<experimental/source_location>)
//     #include <experimental/source_location>
//     #define LOGSTORE_HAS_EXPERIMENTAL_SOURCE_LOCATION 1
// #endif

namespace logstore
{

class category;
class sink;

class source_location_custom
{
  public:
    source_location_custom(char const* filename, int line, char const* functionName) noexcept:
        _fileName { filename }, _line { line }, _functionName { functionName }
    {
    }

    [[nodiscard]] char const* file_name() const noexcept { return _fileName; }
    [[nodiscard]] int line() const noexcept { return _line; }
    [[nodiscard]] char const* function_name() const noexcept { return _functionName; }

    static source_location_custom current() noexcept
    {
        return source_location_custom(__builtin_FILE(), __builtin_LINE(), __builtin_FUNCTION());
    }

  private:
    char const* _fileName;
    int _line;
    char const* _functionName;
};

#if defined __has_include
    #if defined __cpp_lib_source_location
using source_location = std::source_location;
    #else
using source_location = source_location_custom;
    #endif
#endif

class message_builder
{
  private:
    category const& _category;
    source_location _location;
    std::string _buffer;

  public:
    explicit message_builder(category const& cat, source_location loc = source_location::current());

    [[nodiscard]] category const& get_category() const noexcept { return _category; }
    [[nodiscard]] source_location const& location() const noexcept { return _location; }

    [[nodiscard]] std::string const& text() const noexcept { return _buffer; }

    message_builder& append(std::string_view msg)
    {
        _buffer += msg;
        return *this;
    }

    template <typename... T>
    message_builder& append(fmt::format_string<T...> fmt, T&&... args)
    {
        _buffer += fmt::vformat(fmt, fmt::make_format_args(args...));
        return *this;
    }

    message_builder& operator()(std::string const& msg)
    {
        _buffer += msg;
        return *this;
    }
    template <typename... T>
    message_builder& operator()(fmt::format_string<T...> fmt, T&&... args)
    {
        _buffer += fmt::vformat(fmt, fmt::make_format_args(args...));
        return *this;
    }

    [[nodiscard]] std::string message() const;

    ~message_builder();
};

/// Defines a logging category, such as: error, warning, metrics, vt.backend, or renderer.
///
/// A program can have multiple logging categories, all pointing to the same
/// or each to an individual logging sink.
class category
{
  public:
    using formatter = std::function<std::string(message_builder const&)>;
    enum class state
    {
        Enabled,
        Disabled
    };
    enum class visibility
    {
        Public,
        Hidden
    };

    category(std::string_view name,
             std::string_view desc,
             state state = state::Disabled,
             visibility visibility = visibility::Public) noexcept;
    ~category();

    [[nodiscard]] std::string_view name() const noexcept { return _name; }
    [[nodiscard]] std::string_view description() const noexcept { return _description; }

    [[nodiscard]] bool is_enabled() const noexcept { return _state == state::Enabled; }
    void enable(bool enabled = true) noexcept { _state = enabled ? state::Enabled : state::Disabled; }
    void disable() noexcept { _state = state::Disabled; }

    [[nodiscard]] bool visible() const noexcept { return _visibility == visibility::Public; }
    void set_visible(bool visible) { _visibility = visible ? visibility::Public : visibility::Hidden; }

    operator bool() const noexcept { return is_enabled(); }

    [[nodiscard]] formatter const& get_formatter() const { return _formatter; }
    void set_formatter(formatter formatter) { _formatter = std::move(formatter); }

    void set_sink(logstore::sink& s) { _sink = s; }
    [[nodiscard]] logstore::sink& sink() const noexcept { return _sink.get(); }

    [[nodiscard]] message_builder build(source_location location = source_location::current()) const
    {
        return message_builder(*this, location);
    }

    [[nodiscard]] message_builder operator()(source_location location = source_location::current()) const
    {
        return message_builder(*this, location);
    }

    static std::string defaultFormatter(message_builder const& message);

  private:
    std::string_view _name;
    std::string_view _description;
    state _state;
    visibility _visibility;
    formatter _formatter;
    std::reference_wrapper<logstore::sink> _sink;
};

/// Logging sink API.
///
/// Such as the console, a log file, or UDP endpoint.
class sink
{
  public:
    using writer = std::function<void(std::string_view const&)>;

    sink(bool enabled, writer writer);
    sink(bool enabled, std::ostream& output);
    sink(bool enabled, std::shared_ptr<std::ostream> f);

    void set_writer(writer writer);

    /// Writes given built message to this sink.
    void write(message_builder const& message);

    void set_enabled(bool enabled) { _enabled = enabled; }

    /// Retrieves reference to standard debug-logging sink.
    static sink& console();
    static sink& error_console(); // NOLINT(readability-identifier-naming)

  private:
    bool _enabled;
    writer _writer;
};

std::vector<std::reference_wrapper<category>>& get();
category* get(std::string_view categoryName);
void set_sink(sink& sink);
void set_formatter(category::formatter const& f);
void enable(std::string_view categoryName, bool enabled = true);
void disable(std::string_view categoryName);
void configure(std::string_view filterString);

// {{{ implementation
inline std::string message_builder::message() const
{
    if (_category.get_formatter())
        return _category.get_formatter()(*this);
    else if (!_buffer.empty() && _buffer.back() == '\n')
        return _buffer;
    else if (!_buffer.empty())
        return _buffer + '\n';
    else
        return "";
}

inline std::vector<std::reference_wrapper<category>>& get()
{
    static std::vector<std::reference_wrapper<category>> logStore;
    return logStore;
}

inline category* get(std::string_view categoryName)
{
    for (auto const& cat: get())
        if (cat.get().name() == categoryName)
            return &cat.get();
    return nullptr;
}

inline void set_sink(sink& s)
{
    for (auto const& cat: get())
        cat.get().set_sink(s);
}

inline void set_formatter(category::formatter const& f)
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

inline void configure(std::string_view filterString)
{
    if (filterString == "all")
    {
        for (auto& category: logstore::get())
            category.get().enable();
    }
    else
    {
        auto const filters = crispy::split(filterString, ',');
        for (auto& category: logstore::get())
        {
            category.get().enable(crispy::any_of(filters, [&](std::string_view filterPattern) -> bool {
                if (filterPattern.back() != '*')
                    return category.get().name() == filterPattern;
                // TODO: '*' excludes hidden categories
                return std::equal(std::begin(filterPattern),
                                  std::prev(end(filterPattern)),
                                  std::begin(category.get().name()));
            }));
        }
    }
}

inline message_builder::message_builder(logstore::category const& cat, source_location location):
    _category { cat }, _location { location }
{
}

inline message_builder::~message_builder()
{
    _category.sink().write(*this);
}

inline category::category(std::string_view name,
                          std::string_view desc,
                          state state,
                          visibility visibility) noexcept:
    _name { name },
    _description { desc },
    _state { state },
    _visibility { visibility },
    _sink { logstore::sink::console() }
{
    assert(std::none_of(get().begin(), get().end(), [&](category const& x) { return x.name() == _name; }));
    get().emplace_back(*this);
}

inline category::~category()
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

inline std::string category::defaultFormatter(message_builder const& message)
{
    return fmt::format("[{}:{}:{}]: {}\n",
                       message.get_category().name(),
                       message.location().file_name(),
                       message.location().line(),
                       message.text());
}

inline void sink::write(message_builder const& message)
{
    if (_enabled && message.get_category().is_enabled())
        _writer(message.message());
}

inline void sink::set_writer(writer writer)
{
    _writer = std::move(writer);
}
// }}}

auto inline ErrorLog = logstore::category("error", "Error Logger", category::state::Enabled);

#define errorlog() (::logstore::ErrorLog())

} // namespace logstore
