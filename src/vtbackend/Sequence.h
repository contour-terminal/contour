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

#include <vtbackend/Functions.h>

#include <crispy/boxed.h>

#include <gsl/span>

#include <algorithm>
#include <cassert>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace terminal
{

class sequence_parameter_builder;

/**
 * CSI parameter API.
 *
 * This object holds the numeric parameters as used in a CSI sequence.
 *
 * @note Use SequenceParameterBuilder for filling a SequenceParameters object.
 */
class sequence_parameters
{
  public:
    using storage = std::array<uint16_t, 16>;

    [[nodiscard]] constexpr uint16_t at(size_t index) const noexcept { return _values[index]; }

    [[nodiscard]] constexpr bool isSubParameter(size_t index) const noexcept
    {
        return (_subParameterTest & (1 << index)) != 0;
    }

    /// Returns the number of sub-params for a given non-sub param.
    [[nodiscard]] constexpr size_t subParameterCount(size_t index) const noexcept
    {
        if (!isSubParameter(index))
        {
            index++;
            auto const start = index;
            while (index < 16 && isSubParameter(index))
                index++;
            return index - start;
        }
        return 0;
    }

    constexpr void clear() noexcept
    {
        _values[0] = 0;
        _subParameterTest = 0;
        _count = 0;
    }

    [[nodiscard]] constexpr bool empty() const noexcept { return _count == 0; }
    [[nodiscard]] constexpr size_t count() const noexcept { return _count; }

    [[nodiscard]] std::string subParameterBitString() const
    {
        return fmt::format("{:016b}: ", _subParameterTest);
    }

    [[nodiscard]] constexpr gsl::span<std::uint16_t> range() noexcept
    {
        return gsl::span { _values.data(), _count };
    }

    [[nodiscard]] constexpr gsl::span<std::uint16_t const> range() const noexcept
    {
        return gsl::span { _values.data(), _count };
    }

    [[nodiscard]] std::string str() const
    {
        std::string s;

        auto const e = count();
        for (size_t i = 0; i != e; ++i)
        {
            if (!s.empty())
                s += isSubParameter(i) ? ':' : ';';

            if (isSubParameter(i) && !_values[i])
                continue;

            s += std::to_string(_values[i]);
        }

        return s;
    }

  private:
    friend class sequence_parameter_builder;
    storage _values {};
    uint16_t _subParameterTest = 0;
    size_t _count = 0;
};

/**
 * SequenceParameters builder API.
 *
 * Used to progressively fill a SequenceParameters object.
 *
 * @see SequenceParameters
 */
class sequence_parameter_builder
{
  public:
    using storage = sequence_parameters::storage;

    explicit sequence_parameter_builder(sequence_parameters& p):
        _parameters { p }, _currentParameter { p._values.begin() }
    {
    }

    void reset()
    {
        _parameters.clear();
        _currentParameter = _parameters._values.begin();
    }

    void nextParameter()
    {
        if (_currentParameter != _parameters._values.end())
        {
            ++_currentParameter;
            *_currentParameter = 0;
            _parameters._subParameterTest >>= 1;
        }
    }

    void nextSubParameter()
    {
        if (_currentParameter != _parameters._values.end())
        {
            ++_currentParameter;
            *_currentParameter = 0;
            _parameters._subParameterTest = (_parameters._subParameterTest >> 1) | (1 << 15);
        }
    }

    constexpr void multiplyBy10AndAdd(uint8_t value) noexcept
    {
        *_currentParameter = static_cast<uint16_t>(*_currentParameter * 10 + value);
    }

    constexpr void apply(uint16_t value) noexcept
    {
        if (value >= 10)
            multiplyBy10AndAdd(static_cast<uint8_t>(value / 10));
        multiplyBy10AndAdd(static_cast<uint8_t>(value % 10));
    }

    constexpr void set(uint16_t value) noexcept { *_currentParameter = value; }

    [[nodiscard]] constexpr bool isSubParameter(size_t index) const noexcept
    {
        return (_parameters._subParameterTest & (1 << (count() - 1 - index))) != 0;
    }

    [[nodiscard]] constexpr size_t count() const noexcept
    {
        auto const result =
            std::distance(const_cast<sequence_parameter_builder*>(this)->_parameters._values.begin(),
                          _currentParameter)
            + 1;
        if (!(result == 1 && _parameters._values[0] == 0))
            return static_cast<size_t>(result);
        else
            return 0;
    }

    constexpr void fixiate() noexcept
    {
        _parameters._count = count();
        _parameters._subParameterTest >>= 16 - _parameters._count;
    }

  private:
    sequence_parameters& _parameters;
    storage::iterator _currentParameter;
};

/**
 * Helps constructing VT functions as they're being parsed by the VT parser.
 */
class sequence
{
  public:
    size_t constexpr static MaxOscLength = 512; // NOLINT(readability-identifier-naming)

    using parameter = uint16_t;
    using intermediaries = std::string;
    using data_string = std::string;
    using parameters = sequence_parameters;

  private:
    function_category _category = {};
    char _leaderSymbol = 0;
    parameters _parameters;
    intermediaries _intermediateCharacters;
    char _finalChar = 0;
    data_string _dataString;

  public:
    // parameter accessors
    //

    [[nodiscard]] parameters& getParameters() noexcept { return _parameters; }
    [[nodiscard]] parameters const& getParameters() const noexcept { return _parameters; }

    [[nodiscard]] size_t parameterCount() const noexcept { return _parameters.count(); }
    [[nodiscard]] size_t subParameterCount(size_t i) const noexcept
    {
        return _parameters.subParameterCount(i);
    }

    // mutators
    //
    void clear()
    {
        clearExceptParameters();
        _parameters.clear();
    }

    void clearExceptParameters()
    {
        _category = function_category::C0;
        _leaderSymbol = 0;
        _intermediateCharacters.clear();
        _finalChar = 0;
        _dataString.clear();
    }

    void setCategory(function_category cat) noexcept { _category = cat; }
    void setLeader(char ch) noexcept { _leaderSymbol = ch; }
    [[nodiscard]] intermediaries& intermediateCharacters() noexcept { return _intermediateCharacters; }
    void setFinalChar(char ch) noexcept { _finalChar = ch; }

    [[nodiscard]] data_string const& dataString() const noexcept { return _dataString; }
    [[nodiscard]] data_string& dataString() noexcept { return _dataString; }

    /// @returns this VT-sequence into a human readable string form.
    [[nodiscard]] std::string text() const;

    /// @returns the raw VT-sequence string.
    [[nodiscard]] std::string raw() const;

    [[nodiscard]] function_definition const* functionDefinition() const noexcept
    {
        return select(selector());
    }

    /// Converts a FunctionSpinto a FunctionSelector, applicable for finding the corresponding
    /// FunctionDefinition.
    [[nodiscard]] function_selector selector() const noexcept
    {
        switch (_category)
        {
            case function_category::OSC:
                return function_selector {
                    _category, 0, static_cast<int>(parameterCount() ? param(0) : 0), 0, 0
                };
            default: {
                // Only support CSI sequences with 0 or 1 intermediate characters.
                char const intermediate = _intermediateCharacters.size() == 1
                                              ? static_cast<char>(_intermediateCharacters[0])
                                              : char {};

                return function_selector {
                    _category, _leaderSymbol, static_cast<int>(parameterCount()), intermediate, _finalChar
                };
            }
        }
    }

    // accessors
    //
    [[nodiscard]] function_category category() const noexcept { return _category; }
    [[nodiscard]] intermediaries const& intermediateCharacters() const noexcept
    {
        return _intermediateCharacters;
    }
    [[nodiscard]] char leaderSymbol() const noexcept { return _leaderSymbol; }
    [[nodiscard]] char finalChar() const noexcept { return _finalChar; }

    template <typename T = unsigned>
    [[nodiscard]] std::optional<T> param_opt(size_t parameterIndex) const noexcept
    {
        if (parameterIndex < _parameters.count())
        {
            if constexpr (crispy::is_boxed<T>)
                return { T::cast_from(_parameters.at(parameterIndex)) };
            else
                return { static_cast<T>(_parameters.at(parameterIndex)) };
        }
        else
            return std::nullopt;
    }

    template <typename T = unsigned>
    [[nodiscard]] T param_or(size_t parameterIndex, T defaultValue) const noexcept
    {
        return param_opt<T>(parameterIndex).value_or(defaultValue);
    }

    template <typename T = unsigned>
    [[nodiscard]] T param(size_t parameterIndex) const noexcept
    {
        assert(parameterIndex < _parameters.count());
        if constexpr (crispy::is_boxed<T>)
            return T::cast_from(_parameters.at(parameterIndex));
        else
            return static_cast<T>(_parameters.at(parameterIndex));
    }

    template <typename T = unsigned>
    [[nodiscard]] T subparam(size_t parameterIndex, size_t subIndex) const noexcept
    {
        return param<T>(parameterIndex + subIndex);
    }

    [[nodiscard]] bool isSubParameter(size_t parameterIndex) const noexcept
    {
        return _parameters.isSubParameter(parameterIndex);
    }

    template <typename T = unsigned>
    [[nodiscard]] bool containsParameter(T value) const noexcept
    {
        for (size_t i = 0; i < parameterCount(); ++i)
            if constexpr (crispy::is_boxed<T>)
            {
                if (T::cast_from(_parameters.at(i)) == value)
                    return true;
            }
            else
            {
                if (T(_parameters.at(i)) == value)
                    return true;
            }
        return false;
    }
};

class sequence_handler
{
  public:
    virtual ~sequence_handler() = default;

    virtual void executeControlCode(char controlCode) = 0;
    virtual void processSequence(sequence const& sequence) = 0;
    virtual void writeText(char32_t codepoint) = 0;
    virtual void writeText(std::string_view codepoints, size_t cellCount) = 0;
};

} // namespace terminal
