/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2020 Christian Parpart <christian@parpart.family>
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

#include <array>
#include <optional>
#include <utility>

namespace crispy::text {

enum class Core_Property {
    Alphabetic,
    Case_Ignorable,
    Cased,
    Changes_When_Casefolded,
    Changes_When_Casemapped,
    Changes_When_Lowercased,
    Changes_When_Titlecased,
    Changes_When_Uppercased,
    Default_Ignorable_Code_Point,
    Grapheme_Base,
    Grapheme_Extend,
    Grapheme_Link,
    ID_Continue,
    ID_Start,
    Lowercase,
    Math,
    Uppercase,
    XID_Continue,
    XID_Start,
};

bool contains(Core_Property _prop, char32_t _codepoint) noexcept;

enum class General_Category {
    Close_Punctuation,
    Connector_Punctuation,
    Control,
    Currency_Symbol,
    Dash_Punctuation,
    Decimal_Number,
    Enclosing_Mark,
    Final_Punctuation,
    Format,
    Initial_Punctuation,
    Letter_Number,
    Line_Separator,
    Lowercase_Letter,
    Math_Symbol,
    Modifier_Letter,
    Modifier_Symbol,
    Nonspacing_Mark,
    Open_Punctuation,
    Other_Letter,
    Other_Number,
    Other_Punctuation,
    Other_Symbol,
    Paragraph_Separator,
    Private_Use,
    Space_Separator,
    Spacing_Mark,
    Surrogate,
    Titlecase_Letter,
    Unassigned,
    Uppercase_Letter,
};

bool contains(General_Category _cat, char32_t _codepoint) noexcept;

enum class Grapheme_Cluster_Break {
    CR,
    Control,
    Extend,
    L,
    LF,
    LV,
    LVT,
    Prepend,
    Regional_Indicator,
    SpacingMark,
    T,
    V,
    ZWJ,
};

std::optional<Grapheme_Cluster_Break> grapheme_cluster_break(char32_t _codepoint) noexcept;

bool emoji(char32_t _codepoint) noexcept;
bool emoji_component(char32_t _codepoint) noexcept;
bool emoji_modifier(char32_t _codepoint) noexcept;
bool emoji_modifier_base(char32_t _codepoint) noexcept;
bool emoji_presentation(char32_t _codepoint) noexcept;
bool extended_pictographic(char32_t _codepoint) noexcept;

} // end namespace
