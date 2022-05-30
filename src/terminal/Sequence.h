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

#include <terminal/Functions.h>
#include <terminal/ParserExtension.h>

#include <crispy/boxed.h>

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

class SequenceParameterBuilder;

/**
 * CSI parameter API.
 *
 * This object holds the numeric parameters as used in a CSI sequence.
 *
 * @note Use SequenceParameterBuilder for filling a SequenceParameters object.
 */
class SequenceParameters
{
  public:
    using Storage = std::array<uint16_t, 16>;

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
    friend class SequenceParameterBuilder;
    Storage _values {};
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
class SequenceParameterBuilder
{
  public:
    using Storage = SequenceParameters::Storage;

    explicit SequenceParameterBuilder(SequenceParameters& p):
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
            std::distance(const_cast<SequenceParameterBuilder*>(this)->_parameters._values.begin(),
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
    SequenceParameters& _parameters;
    Storage::iterator _currentParameter;
};

/**
 * Helps constructing VT functions as they're being parsed by the VT parser.
 */
class Sequence
{
  public:
    size_t constexpr static MaxOscLength = 512;

    using Parameter = uint16_t;
    using Intermediaries = std::string;
    using DataString = std::string;
    using Parameters = SequenceParameters;

  private:
    FunctionCategory category_;
    char leaderSymbol_ = 0;
    Parameters parameters_;
    Intermediaries intermediateCharacters_;
    char finalChar_ = 0;
    DataString dataString_;

  public:
    // parameter accessors
    //

    [[nodiscard]] Parameters& parameters() noexcept { return parameters_; }
    [[nodiscard]] Parameters const& parameters() const noexcept { return parameters_; }

    [[nodiscard]] size_t parameterCount() const noexcept { return parameters_.count(); }
    [[nodiscard]] size_t subParameterCount(size_t i) const noexcept
    {
        return parameters_.subParameterCount(i);
    }

    // mutators
    //
    void clear()
    {
        clearExceptParameters();
        parameters_.clear();
    }

    void clearExceptParameters()
    {
        category_ = FunctionCategory::C0;
        leaderSymbol_ = 0;
        intermediateCharacters_.clear();
        finalChar_ = 0;
        dataString_.clear();
    }

    void setCategory(FunctionCategory _cat) noexcept { category_ = _cat; }
    void setLeader(char _ch) noexcept { leaderSymbol_ = _ch; }
    [[nodiscard]] Intermediaries& intermediateCharacters() noexcept { return intermediateCharacters_; }
    void setFinalChar(char _ch) noexcept { finalChar_ = _ch; }

    [[nodiscard]] DataString const& dataString() const noexcept { return dataString_; }
    [[nodiscard]] DataString& dataString() noexcept { return dataString_; }

    /// @returns this VT-sequence into a human readable string form.
    [[nodiscard]] std::string text() const;

    /// @returns the raw VT-sequence string.
    [[nodiscard]] std::string raw() const;

    [[nodiscard]] FunctionDefinition const* functionDefinition() const noexcept { return select(selector()); }

    /// Converts a FunctionSpinto a FunctionSelector, applicable for finding the corresponding
    /// FunctionDefinition.
    [[nodiscard]] FunctionSelector selector() const noexcept
    {
        switch (category_)
        {
            case FunctionCategory::OSC:
                return FunctionSelector {
                    category_, 0, static_cast<int>(parameterCount() ? param(0) : 0), 0, 0
                };
            default: {
                // Only support CSI sequences with 0 or 1 intermediate characters.
                char const intermediate = intermediateCharacters_.size() == 1
                                              ? static_cast<char>(intermediateCharacters_[0])
                                              : char {};

                return FunctionSelector {
                    category_, leaderSymbol_, static_cast<int>(parameterCount()), intermediate, finalChar_
                };
            }
        }
    }

    // accessors
    //
    [[nodiscard]] FunctionCategory category() const noexcept { return category_; }
    [[nodiscard]] Intermediaries const& intermediateCharacters() const noexcept
    {
        return intermediateCharacters_;
    }
    [[nodiscard]] char finalChar() const noexcept { return finalChar_; }

    template <typename T = unsigned>
    [[nodiscard]] std::optional<T> param_opt(size_t parameterIndex) const noexcept
    {
        if (parameterIndex < parameters_.count())
        {
            if constexpr (crispy::is_boxed<T>)
                return { T::cast_from(parameters_.at(parameterIndex)) };
            else
                return { static_cast<T>(parameters_.at(parameterIndex)) };
        }
        else
            return std::nullopt;
    }

    template <typename T = unsigned>
    [[nodiscard]] T param_or(size_t parameterIndex, T _defaultValue) const noexcept
    {
        return param_opt<T>(parameterIndex).value_or(_defaultValue);
    }

    template <typename T = unsigned>
    [[nodiscard]] T param(size_t parameterIndex) const noexcept
    {
        assert(parameterIndex < parameters_.count());
        if constexpr (crispy::is_boxed<T>)
            return T::cast_from(parameters_.at(parameterIndex));
        else
            return static_cast<T>(parameters_.at(parameterIndex));
    }

    template <typename T = unsigned>
    [[nodiscard]] T subparam(size_t parameterIndex, size_t subIndex) const noexcept
    {
        return param<T>(parameterIndex + subIndex);
    }

    [[nodiscard]] bool isSubParameter(size_t parameterIndex) const noexcept
    {
        return parameters_.isSubParameter(parameterIndex);
    }

    template <typename T = unsigned>
    [[nodiscard]] bool containsParameter(T _value) const noexcept
    {
        for (size_t i = 0; i < parameterCount(); ++i)
            if constexpr (crispy::is_boxed<T>)
            {
                if (T::cast_from(parameters_.at(i)) == _value)
                    return true;
            }
            else
            {
                if (T(parameters_.at(i)) == _value)
                    return true;
            }
        return false;
    }
};

class SequenceHandler
{
  public:
    virtual ~SequenceHandler() = default;

    virtual void executeControlCode(char controlCode) = 0;
    virtual void processSequence(Sequence const& sequence) = 0;
    virtual void writeText(char32_t codepoint) = 0;
    virtual void writeText(std::string_view codepoints, size_t cellCount) = 0;
};

} // namespace terminal
