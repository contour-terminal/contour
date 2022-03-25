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

#include <crispy/boxed.h>

#include <cassert>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace terminal
{

template <typename T, size_t MaxParameters, size_t MaxSubParameters>
class SequenceParameters
{
  public:
    static_assert(MaxParameters >= 1);
    static_assert(MaxSubParameters >= 1);

    constexpr T& at(size_t i, size_t j = 0) { return _values.at(i * MaxSubParameters + j); }
    constexpr T const& at(size_t i, size_t j = 0) const { return _values.at(i * MaxSubParameters + j); }

    constexpr void clear()
    {
        _size = 0;
        for (auto i = _subParameterCounts.begin(), e = _subParameterCounts.end(); i != e; ++i)
            *i = 0;
    }

    constexpr bool empty() const noexcept { return _size == 0; }
    constexpr size_t size() const noexcept { return _size; }
    size_t subParameterCount(size_t i) const noexcept { return _subParameterCounts[i]; }

    constexpr void appendParameter(T value) noexcept { set(_size++, 0, value); }
    constexpr void appendSubParameter(T value) noexcept
    {
        set(_size - 1, ++_subParameterCounts[_size], value);
    }

    constexpr void set(size_t i, size_t j, unsigned value) noexcept
    {
        if (i < MaxParameters && j < MaxSubParameters)
            _values[i * MaxSubParameters + j] = value;
    }

    /// Shifts the parameter at (i, j) by one decimal position to the left (multiply by 10)
    /// and then add @p value to it.
    constexpr void multiplyBy10AndAdd(size_t i, size_t j, unsigned value) noexcept
    {
        if (i < MaxParameters && j < MaxSubParameters)
        {
            auto const offset = i * MaxSubParameters + j;
            _values[offset] = _values[offset] * 10 + value;
        }
    }

    std::string str() const
    {
        std::string s;

        for (size_t i = 0; i < size(); ++i)
        {
            if (i)
                s += ';';

            s += std::to_string(at(i));
            for (size_t j = 1; j < subParameterCount(i); ++j)
            {
                if (j)
                    s += ':';
                s += std::to_string(at(i, j));
            }
        }

        return s;
    }

  private:
    std::array<T, MaxParameters * MaxSubParameters> _values {};
    std::array<size_t, MaxParameters> _subParameterCounts {};
    size_t _size = 0;
};

/// Helps constructing VT functions as they're being parsed by the VT parser.
class Sequence
{
  public:
    size_t constexpr static MaxParameters = 8;
    size_t constexpr static MaxSubParameters = 6;
    size_t constexpr static MaxOscLength = 512;

    using Parameter = uint32_t;
    using Intermediaries = std::string;
    using DataString = std::string;
    using Parameters = SequenceParameters<Parameter, MaxParameters, MaxSubParameters>;

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

    Parameters& parameters() noexcept { return parameters_; }
    Parameters const& parameters() const noexcept { return parameters_; }

    size_t parameterCount() const noexcept { return parameters_.size(); }
    size_t subParameterCount(size_t i) const noexcept { return parameters_.subParameterCount(i); }

    // mutators
    //
    void clear()
    {
        category_ = FunctionCategory::C0;
        leaderSymbol_ = 0;
        intermediateCharacters_.clear();
        parameters_.clear();
        finalChar_ = 0;
        dataString_.clear();
    }

    void setCategory(FunctionCategory _cat) noexcept { category_ = _cat; }
    void setLeader(char _ch) noexcept { leaderSymbol_ = _ch; }
    Intermediaries& intermediateCharacters() noexcept { return intermediateCharacters_; }
    void setFinalChar(char _ch) noexcept { finalChar_ = _ch; }

    DataString const& dataString() const noexcept { return dataString_; }
    DataString& dataString() noexcept { return dataString_; }

    /// @returns this VT-sequence into a human readable string form.
    std::string text() const;

    /// @returns the raw VT-sequence string.
    std::string raw() const;

    FunctionDefinition const* functionDefinition() const noexcept { return select(selector()); }

    /// Converts a FunctionSpinto a FunctionSelector, applicable for finding the corresponding
    /// FunctionDefinition.
    FunctionSelector selector() const noexcept
    {
        switch (category_)
        {
            case FunctionCategory::OSC:
                return FunctionSelector { category_, 0, static_cast<int>(param(0)), 0, 0 };
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
    FunctionCategory category() const noexcept { return category_; }
    Intermediaries const& intermediateCharacters() const noexcept { return intermediateCharacters_; }
    char finalChar() const noexcept { return finalChar_; }

    template <typename T = unsigned>
    std::optional<T> param_opt(size_t parameterIndex) const noexcept
    {
        if (parameterIndex < parameters_.size())
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
    T param_or(size_t parameterIndex, T _defaultValue) const noexcept
    {
        return param_opt<T>(parameterIndex).value_or(_defaultValue);
    }

    template <typename T = unsigned>
    T param(size_t parameterIndex) const noexcept
    {
        assert(parameterIndex < parameters_.size());
        // TODO(pr)? assert(0 < parameters_[parameterIndex].size());
        if constexpr (crispy::is_boxed<T>)
            return T::cast_from(parameters_.at(parameterIndex));
        else
            return static_cast<T>(parameters_.at(parameterIndex));
    }

    template <typename T = unsigned>
    T subparam(size_t parameterIndex, size_t subIndex) const noexcept
    {
        assert(parameterIndex < parameters_.size());
        assert(subIndex < subParameterCount(parameterIndex));
        if constexpr (crispy::is_boxed<T>)
            return T::cast_from(parameters_.at(parameterIndex, subIndex));
        else
            return static_cast<T>(parameters_.at(parameterIndex, subIndex));
    }

    template <typename T = unsigned>
    bool containsParameter(T _value) const noexcept
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

} // namespace terminal
