// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/Functions.h>

#include <gsl/pointers>
#include <gsl/span>

#include <array>
#include <cassert>
#include <concepts>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>

#include <boxed-cpp/boxed.hpp>

namespace vtbackend
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

    /// @return Whether parameter @p index was given, but empty -- as the first one of `CSI ; 5 H` is.
    ///
    /// An omitted parameter is stored as the value zero, so without this the two cannot be told apart.
    /// Most sequences do not care, because zero *is* their default; XTWINOPS is the one that does, where
    /// an omitted dimension keeps the window's current size and a zero one grows it to the display's.
    [[nodiscard]] constexpr bool isOmitted(size_t index) const noexcept
    {
        return index < _count && (_omittedTest & (1 << index)) != 0;
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
        _omittedTest = AllOmitted;
        _count = 0;
    }

    [[nodiscard]] constexpr bool empty() const noexcept { return _count == 0; }
    [[nodiscard]] constexpr size_t count() const noexcept { return _count; }

    [[nodiscard]] std::string subParameterBitString() const
    {
        return std::format("{:016b}: ", _subParameterTest);
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
    friend class SequenceParameterBuilder;

    /// Every parameter starts out omitted, and stops being so the moment a digit lands on it.
    static constexpr uint16_t AllOmitted = 0xFFFF;

    Storage _values {};
    uint16_t _subParameterTest = 0;
    uint16_t _omittedTest = AllOmitted;
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
        _parameters { &p }, _currentParameter { p._values.begin() }
    {
    }

    void reset()
    {
        _parameters->clear();
        _currentParameter = _parameters->_values.begin();
        _currentIndex = 0;
    }

    void nextParameter()
    {
        if (std::next(_currentParameter) != _parameters->_values.end())
        {
            ++_currentParameter;
            ++_currentIndex;
            *_currentParameter = 0;
            _parameters->_subParameterTest >>= 1;
        }
    }

    void nextSubParameter()
    {
        if (std::next(_currentParameter) != _parameters->_values.end())
        {
            ++_currentParameter;
            ++_currentIndex;
            *_currentParameter = 0;
            _parameters->_subParameterTest = (_parameters->_subParameterTest >> 1) | (1 << 15);
        }
    }

    /// Records that the parameter being built carries a value, and is therefore not an omitted one.
    constexpr void markPresent() noexcept
    {
        if (_currentIndex < std::tuple_size_v<Storage>)
            _parameters->_omittedTest &= static_cast<uint16_t>(~(1u << _currentIndex));
    }

    constexpr void multiplyBy10AndAdd(uint8_t value) noexcept
    {
        markPresent();
        unsigned const newValue = (*_currentParameter * 10) + value;
        if (newValue > 0xFFFF)
            *_currentParameter = 0xFFFF;
        else
            *_currentParameter = static_cast<uint16_t>(newValue);
    }

    constexpr void apply(uint16_t value) noexcept
    {
        if (value >= 10)
            multiplyBy10AndAdd(static_cast<uint8_t>(value / 10));
        multiplyBy10AndAdd(static_cast<uint8_t>(value % 10));
    }

    constexpr void set(uint16_t value) noexcept
    {
        markPresent();
        *_currentParameter = value;
    }

    [[nodiscard]] constexpr bool isSubParameter(size_t index) const noexcept
    {
        return (_parameters->_subParameterTest & (1 << (count() - 1 - index))) != 0;
    }

    [[nodiscard]] constexpr size_t count() const noexcept
    {
        auto const result =
            std::distance(const_cast<SequenceParameterBuilder*>(this)->_parameters->_values.begin(),
                          _currentParameter)
            + 1;
        if (!(result == 1 && _parameters->_values[0] == 0))
            return static_cast<size_t>(result);
        else
            return 0;
    }

    constexpr void fixiate() noexcept
    {
        _parameters->_count = count();
        _parameters->_subParameterTest >>= 16 - _parameters->_count;
    }

  private:
    gsl::not_null<SequenceParameters*> _parameters;
    Storage::iterator _currentParameter;

    /// Index of @c _currentParameter, kept alongside the iterator so that the omitted-parameter mask can
    /// be addressed without recomputing a distance on every digit.
    size_t _currentIndex = 0;
};

/**
 * Helps constructing VT functions as they're being parsed by the VT parser.
 */
class Sequence
{
  public:
    // Make maximum size 50 kB since we need to support adding to the clipboard
    // and the clipboard can contain large amounts of text.
    size_t constexpr static MaxOscLength = 1024 * 50; // NOLINT(readability-identifier-naming)

    using Parameter = uint16_t;
    using Intermediaries = std::string;
    using DataString = std::string;
    using Parameters = SequenceParameters;

  private:
    FunctionCategory _category = {};
    char _leaderSymbol = 0;
    Parameters _parameters;
    Intermediaries _intermediateCharacters;
    char _finalChar = 0;
    DataString _dataString;

  public:
    // parameter accessors
    //

    [[nodiscard]] Parameters& parameters() noexcept { return _parameters; }
    [[nodiscard]] Parameters const& parameters() const noexcept { return _parameters; }

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
        _category = FunctionCategory::C0;
        _leaderSymbol = 0;
        _intermediateCharacters.clear();
        _finalChar = 0;
        _dataString.clear();
    }

    void setCategory(FunctionCategory cat) noexcept { _category = cat; }
    void setLeader(char ch) noexcept { _leaderSymbol = ch; }
    [[nodiscard]] Intermediaries& intermediateCharacters() noexcept { return _intermediateCharacters; }
    void setFinalChar(char ch) noexcept { _finalChar = ch; }

    [[nodiscard]] DataString const& dataString() const noexcept { return _dataString; }
    [[nodiscard]] DataString& dataString() noexcept { return _dataString; }

    /// @returns this VT-sequence into a human readable string form.
    [[nodiscard]] std::string text() const;

    /// @returns the raw VT-sequence string.
    [[nodiscard]] std::string raw() const;

    [[nodiscard]] Function const* functionDefinition(
        gsl::span<Function const> availableDefinitions) const noexcept
    {
        return select(selector(), availableDefinitions);
    }

    /// Converts a FunctionSpinto a FunctionSelector, applicable for finding the corresponding
    /// FunctionDefinition.
    [[nodiscard]] FunctionSelector selector() const noexcept
    {
        switch (_category)
        {
            case FunctionCategory::OSC:
                return FunctionSelector { .category = _category,
                                          .leader = 0,
                                          .argc = static_cast<int>(parameterCount() ? param(0) : 0),
                                          .intermediate = 0,
                                          .finalSymbol = 0 };
            default: {
                // Only support CSI sequences with 0 or 1 intermediate characters.
                char const intermediate = _intermediateCharacters.size() == 1
                                              ? static_cast<char>(_intermediateCharacters[0])
                                              : char {};

                return FunctionSelector { .category = _category,
                                          .leader = _leaderSymbol,
                                          .argc = static_cast<int>(parameterCount()),
                                          .intermediate = intermediate,
                                          .finalSymbol = _finalChar };
            }
        }
    }

    // accessors
    //
    [[nodiscard]] FunctionCategory category() const noexcept { return _category; }
    [[nodiscard]] Intermediaries const& intermediateCharacters() const noexcept
    {
        return _intermediateCharacters;
    }
    [[nodiscard]] char leaderSymbol() const noexcept { return _leaderSymbol; }
    [[nodiscard]] char finalChar() const noexcept { return _finalChar; }

    /// @return The value of parameter @p parameterIndex, or std::nullopt if it was never given.
    ///
    /// A parameter is "never given" both when it lies past the end of the list -- `CSI H` names no row --
    /// and when it was given empty: `CSI ; 5 H` names no row either. ECMA-48 makes no distinction
    /// between the two, and neither does this.
    template <typename T = unsigned>
    [[nodiscard]] std::optional<T> param_opt(size_t parameterIndex) const noexcept
    {
        if (parameterIndex >= _parameters.count() || _parameters.isOmitted(parameterIndex))
            return std::nullopt;

        if constexpr (boxed::is_boxed<T>)
            return { T::cast_from(_parameters.at(parameterIndex)) };
        else
            return { static_cast<T>(_parameters.at(parameterIndex)) };
    }

    template <typename T = unsigned>
    [[nodiscard]] T param_or(size_t parameterIndex, T defaultValue) const noexcept
    {
        return param_opt<T>(parameterIndex).value_or(defaultValue);
    }

    /// Reads a parameter whose values start at one -- a coordinate, or a count of something.
    ///
    /// Such a parameter takes its default value in two cases, and the caller must not be made to tell
    /// them apart:
    ///
    ///   - It was never given: `CSI H` names no row at all.
    ///   - It was given empty: `CSI ; 5 H` names no row either, but the parser stores an omitted
    ///     parameter as the value zero and counts it, so param_or() would hand back that zero rather
    ///     than the default.
    ///
    /// Folding zero onto the default covers both, and is what DEC's terminals and xterm
    /// (`if (param < 1) param = 1`) do anyway: there is no row zero and no column zero, so a sequence
    /// naming one is naming the default. Reading such a parameter with param_or() instead invites the
    /// caller to compute `param - 1` and underflow into a negative offset.
    ///
    /// @param parameterIndex Zero-based index of the parameter.
    /// @param defaultValue   Value to use when the parameter is omitted, empty, or zero.
    /// @return The parameter's value, or @p defaultValue.
    template <typename T = unsigned>
    [[nodiscard]] T param_positive_or(size_t parameterIndex, T defaultValue) const noexcept
    {
        auto const value = param_opt<T>(parameterIndex);
        if (!value.has_value() || *value == T {})
            return defaultValue;
        return *value;
    }

    template <typename T = unsigned>
    [[nodiscard]] T param(size_t parameterIndex) const noexcept
    {
        assert(parameterIndex < _parameters.count());
        if constexpr (boxed::is_boxed<T>)
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
            if constexpr (boxed::is_boxed<T>)
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

class SequenceHandler
{
  public:
    virtual ~SequenceHandler() = default;

    virtual void executeControlCode(char controlCode) = 0;
    virtual void processSequence(Sequence const& sequence) = 0;
    virtual void writeText(char32_t codepoint) = 0;
    virtual void writeText(std::string_view codepoints, size_t cellCount) = 0;
    virtual void writeTextEnd() = 0;
};

template <typename T>
concept SequenceHandlerConcept = requires(T t) {
    { t.executeControlCode('\x00') } -> std::same_as<void>;
    { t.processSequence(Sequence {}) } -> std::same_as<void>;
    { t.writeText(U'a') } -> std::same_as<void>;
    { t.writeText("a", size_t(1)) } -> std::same_as<void>;
    { t.writeTextEnd() } -> std::same_as<void>;
};

} // namespace vtbackend
