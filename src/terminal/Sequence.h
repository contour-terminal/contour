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
// #include <terminal/primitives.h>

#include <cassert>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace terminal {

/// Helps constructing VT functions as they're being parsed by the VT parser.
class Sequence {
  public:
    using Parameter = unsigned;
    using ParameterList = std::vector<std::vector<Parameter>>;
    using Intermediaries = std::string;
    using DataString = std::string;

  private:
    FunctionCategory category_;
    char leaderSymbol_ = 0;
    ParameterList parameters_;
    Intermediaries intermediateCharacters_;
    char finalChar_ = 0;
    DataString dataString_;

  public:
    size_t constexpr static MaxParameters = 16;
    size_t constexpr static MaxSubParameters = 8;
    size_t constexpr static MaxOscLength = 512;

    Sequence()
    {
        parameters_.resize(MaxParameters);
        for (auto& param : parameters_)
            param.reserve(MaxSubParameters);
        parameters_.clear();
    }

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
    ParameterList& parameters() noexcept { return parameters_; }
    Intermediaries& intermediateCharacters() noexcept { return intermediateCharacters_; }
    void setFinalChar(char _ch) noexcept { finalChar_ = _ch; }

    DataString const& dataString() const noexcept { return dataString_; }
    DataString& dataString() noexcept { return dataString_; }

    /// @returns this VT-sequence into a human readable string form.
    std::string text() const;

    /// @returns the raw VT-sequence string.
    std::string raw() const;

    FunctionDefinition const* functionDefinition() const noexcept
    {
        return select(selector());
    }

    /// Converts a FunctionSpinto a FunctionSelector, applicable for finding the corresponding FunctionDefinition.
    FunctionSelector selector() const noexcept
    {
        switch (category_)
        {
            case FunctionCategory::OSC:
                return FunctionSelector{category_, 0, static_cast<int>(parameters_[0][0]), 0, 0};
            default:
            {
                // Only support CSI sequences with 0 or 1 intermediate characters.
                char const intermediate = intermediateCharacters_.size() == 1
                    ? static_cast<char>(intermediateCharacters_[0])
                    : char{};

                return FunctionSelector{category_, leaderSymbol_, static_cast<int>(parameters_.size()), intermediate, finalChar_};
            }
        }
    }

    // accessors
    //
    FunctionCategory category() const noexcept { return category_; }
    Intermediaries const& intermediateCharacters() const noexcept { return intermediateCharacters_; }
    char finalChar() const noexcept { return finalChar_; }

    ParameterList const& parameters() const noexcept { return parameters_; }
    size_t parameterCount() const noexcept { return parameters_.size(); }
    size_t subParameterCount(size_t _index) const noexcept { return parameters_[_index].size() - 1; }

    template <typename T = unsigned>
    std::optional<T> param_opt(size_t _index) const noexcept
    {
        if (_index < parameters_.size() && parameters_[_index][0])
            return {static_cast<T>(parameters_[_index][0])};
        else
            return std::nullopt;
    }

    template <typename T = unsigned>
    T param_or(size_t _index, T _defaultValue) const noexcept
    {
        return param_opt<T>(_index).value_or(_defaultValue);
    }

    template <typename T = unsigned>
    T param(size_t _index) const noexcept
    {
        assert(_index < parameters_.size());
        assert(0 < parameters_[_index].size());
        return T(parameters_[_index][0]);
    }

    template <typename T = unsigned>
    T subparam(size_t _index, size_t _subIndex) const noexcept
    {
        assert(_index < parameters_.size());
        assert(_subIndex + 1 < parameters_[_index].size());
        return T(parameters_[_index][_subIndex + 1]);
    }

    template <typename T = unsigned>
    bool containsParameter(T _value) const noexcept
    {
        for (size_t i = 0; i < parameterCount(); ++i)
            if (T(parameters_[i][0]) == _value)
                return true;
        return false;
    }
};

}
