#! /usr/bin/env python3
"""/**
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
"""

import re

PROJECT_ROOT = '/home/trapni/projects/contour'

# unicode database (extracted zip file): https://www.unicode.org/Public/UCD/latest/ucd/
UCD_DIR = PROJECT_ROOT + '/docs/ucd'

DERIVED_CORE_PROPERTIES_FILE = UCD_DIR + '/DerivedCoreProperties.txt'
GRAPHEME_BREAK_PROPS_FILE = UCD_DIR + '/./auxiliary/GraphemeBreakProperty.txt'
DERIVED_GENERAL_CATEGORY_FILE = UCD_DIR + '/extracted/DerivedGeneralCategory.txt'
EXTENDED_PICTOGRAPHIC_FILE = UCD_DIR + '/emoji/emoji-data.txt'

TARGET_HEADER_FILE = PROJECT_ROOT + '/src/crispy/text/Unicode.h'
TARGET_IMPL_FILE = PROJECT_ROOT + '/src/crispy/text/Unicode.cpp'

def file_header(header, impl):
    header.write(globals()['__doc__'])
    header.write("""#pragma once

#include <array>
#include <optional>
#include <utility>

namespace crispy::text {

""")

    impl.write(globals()['__doc__'])
    impl.write("""
#include <crispy/text/Unicode.h>

#include <array>
#include <optional>

namespace crispy::text {

namespace {
    struct Interval
    {
        char32_t from;
        char32_t to;
    };

    template <size_t N>
    constexpr bool contains(std::array<Interval, N> const& _ranges, char32_t _codepoint)
    {
        int a = 0;
        int b = _ranges.size() - 1;
        while (a < b)
        {
            size_t i = (b + a) / 2;
            auto const& I = _ranges[i];
            if (I.to < _codepoint)
                a = i + 1;
            else if (I.from > _codepoint)
                b = i - 1;
            else
                return true;
        }
        return a == b && _ranges[a].from <= _codepoint && _codepoint <= _ranges[a].to;
    }

    template <typename T> struct Prop
    {
        Interval interval;
        T property;
    };

    template <typename T, size_t N>
    constexpr std::optional<T> search(std::array<Prop<T>, N> const& _ranges, char32_t _codepoint)
    {
        size_t a = 0;
        size_t b = _ranges.size() - 1;
        while (a < b)
        {
            size_t i = (b + a) / 2;
            auto const& I = _ranges[i];
            if (I.interval.to < _codepoint)
                a = i + 1;
            else if (I.interval.from > _codepoint)
                b = i - 1;
            else
                return I.property;
        }
        if (a == b && _ranges[a].interval.from <= _codepoint && _codepoint <= _ranges[a].interval.to)
            return _ranges[a].property;
        return std::nullopt;
    }
}

""")

def file_footer(header, impl):
    header.write("} // end namespace\n")
    impl.write("} // end namespace\n")

def process_core_props(header, impl):
    with open(DERIVED_CORE_PROPERTIES_FILE, 'r') as f:
        singleValueRE = re.compile('([0-9A-F]+)\s*;\s*(\w+)\s*#\s*(.*)$')
        rangeValueRE = re.compile('([0-9A-F]+)\.\.([0-9A-F]+)\s*;\s*(\w+)\s*#\s*(.*)$')

        # collect
        props = dict()
        while True:
            line = f.readline()
            if not line:
                break
            if len(line) == 0 or line[0] == '#':
                continue
            m = singleValueRE.match(line)
            if m:
                code = int(m.group(1), 16)
                prop = m.group(2)
                comment = m.group(3)
                if not (prop in props):
                    props[prop] = []
                props[prop].append({'start': code, 'end': code, 'comment': comment})
            m = rangeValueRE.match(line)
            if m:
                start = int(m.group(1), 16)
                end = int(m.group(2), 16)
                prop = m.group(3)
                comment = m.group(4)
                if not (prop in props):
                    props[prop] = []
                props[prop].append({'start': start, 'end': end, 'comment': comment})

        # sort table
        for prop_key in props.keys():
                props[prop_key].sort(key = lambda a: a['start'])

        # write range tables
        impl.write("namespace tables {\n")
        for name in sorted(props.keys()):
            impl.write("auto constexpr {} = std::array{{ // {{{{{{\n".format(name))
            for propRange in props[name]:
                impl.write("    Interval{{ 0x{:>04X}, 0x{:>04X} }}, // {}\n".format(propRange['start'], propRange['end'], propRange['comment']))
            impl.write("}; // }}}\n")
        impl.write("} // end namespace tables\n\n")

        # write out test function
        impl.write("bool contains(Core_Property _prop, char32_t _codepoint) noexcept {\n")
        impl.write("    switch (_prop) {\n")
        for name in sorted(props.keys()):
            impl.write("        case Core_Property::{0:}: return contains(tables::{0:}, _codepoint);\n".format(name))
        impl.write("    }\n")
        impl.write("    return false;\n")
        impl.write("}\n\n")

        # API: write enum and tester
        header.write("enum class Core_Property {\n")
        for name in sorted(props.keys()):
            header.write("    {},\n".format(name))
        header.write("};\n\n")
        header.write("bool contains(Core_Property _prop, char32_t _codepoint) noexcept;\n\n")

def process_props(header, impl, filename, prop_key):
    with open(filename, 'r') as f:
        # General_Category=Spacing_Mark
        headerRE = re.compile('^#\s*{}:\s*(\w+)$'.format(prop_key))
        singleValueRE = re.compile('([0-9A-F]+)\s*;\s*(\w+)\s*#\s*(.*)$')
        rangeValueRE = re.compile('([0-9A-F]+)\.\.([0-9A-F]+)\s*;\s*(\w+)\s*#\s*(.*)$')

        # collect
        props_name = ''
        props = dict()
        while True:
            line = f.readline()
            if not line:
                break
            m = headerRE.match(line)
            if m:
                props_name = m.group(1)
                if not (props_name in props):
                    props[props_name] = []
            m = singleValueRE.match(line)
            if m:
                code = int(m.group(1), 16)
                prop = m.group(2)
                comment = m.group(3)
                props[props_name].append({'start': code, 'end': code, 'property': prop, 'comment': comment})
            m = rangeValueRE.match(line)
            if m:
                start = int(m.group(1), 16)
                end = int(m.group(2), 16)
                prop = m.group(3)
                comment = m.group(4)
                props[props_name].append({'start': start, 'end': end, 'property': prop, 'comment': comment})

        # sort table
        for prop_key in props.keys():
                props[prop_key].sort(key = lambda a: a['start'])

        # write range tables
        impl.write("namespace tables {\n")
        for name in sorted(props.keys()):
            impl.write("auto constexpr {} = std::array{{ // {{{{{{\n".format(name))
            for propRange in props[name]:
                impl.write("    Prop<crispy::text::{}>{{ {{ 0x{:>04X}, 0x{:>04X} }}, crispy::text::{}::{} }}, // {}\n".format(
                           name,
                           propRange['start'],
                           propRange['end'],
                           name,
                           propRange['property'],
                           propRange['comment']))
            impl.write("}; // }}}\n")
        impl.write("} // end namespace tables\n\n")

        # write out test function
        for name in sorted(props.keys()):
            impl.write('std::optional<{}> {}(char32_t _codepoint) noexcept {{\n'.format(name, name.lower()))
            impl.write("    return search(tables::{0:}, _codepoint);\n".format(name))
            impl.write("}\n\n")

        # write enums / signature
        for name in sorted(props.keys()):
            header.write('enum class {} {{\n'.format(name))
            enums = set()
            for enum in props[name]:
                enums.add(enum['property'])
            for enum in sorted(enums):
                header.write("    {},\n".format(enum))
            header.write("};\n\n")
            header.write('std::optional<{}> {}(char32_t _codepoint) noexcept;\n'.format(name, name.lower()))
        header.write('\n')

def process_grapheme_break_props(header, impl):
    process_props(header, impl, GRAPHEME_BREAK_PROPS_FILE, 'Property')

def parse_range(line):
    singleValueRE = re.compile('([0-9A-F]+)\s*;\s*(\w+)\s*#\s*(.*)$')
    rangeValueRE = re.compile('([0-9A-F]+)\.\.([0-9A-F]+)\s*;\s*(\w+)\s*#\s*(.*)$')
    m = singleValueRE.match(line)
    if m:
        code = int(m.group(1), 16)
        prop = m.group(2)
        comment = m.group(3)
        return {'start': code, 'end': code, 'property': prop, 'comment': comment}
    m = rangeValueRE.match(line)
    if m:
        start = int(m.group(1), 16)
        end = int(m.group(2), 16)
        prop = m.group(3)
        comment = m.group(4)
        return {'start': start, 'end': end, 'property': prop, 'comment': comment}
    return None

def process_emoji_props(header, impl):
    with open(EXTENDED_PICTOGRAPHIC_FILE, 'r') as f:
        # collect
        props_name = ''
        props = dict()
        while True:
            line = f.readline()
            if not line:
                break
            r = parse_range(line)
            if r != None:
                name = r['property']
                if not name in props:
                    props[name] = []
                props[name].append(r)

        # sort table
        for prop_key in props.keys():
                props[prop_key].sort(key = lambda a: a['start'])

        # write range tables
        impl.write("namespace tables {\n")
        for name in sorted(props.keys()):
            impl.write("auto constexpr {} = std::array{{ // {{{{{{\n".format(name))
            for propRange in props[name]:
                impl.write("    Interval{{ 0x{:>04X}, 0x{:>04X} }}, // {}\n".format(
                           propRange['start'],
                           propRange['end'],
                           propRange['comment']))
            impl.write("}; // }}}\n")
        impl.write("} // end namespace tables\n\n")

        # write out test function
        for name in sorted(props.keys()):
            impl.write('bool {}(char32_t _codepoint) noexcept {{\n'.format(name.lower()))
            impl.write("    return contains(tables::{0:}, _codepoint);\n".format(name))
            impl.write("}\n\n")

        # write enums / signature
        for name in sorted(props.keys()):
            # header.write('enum class {} {{\n'.format(name))
            # enums = set()
            # for enum in props[name]:
            #     enums.add(enum['property'])
            # for enum in sorted(enums):
            #     header.write("    {},\n".format(enum))
            # header.write("};\n\n")
            header.write('bool {}(char32_t _codepoint) noexcept;\n'.format(name.lower()))
        header.write('\n')

def process_derived_general_category(header, impl):
    with open(DERIVED_GENERAL_CATEGORY_FILE, 'r') as f:
        # General_Category=Spacing_Mark
        headerRE = re.compile('^#\s*General_Category=(\w+)$')
        singleValueRE = re.compile('([0-9A-F]+)\s*;\s*(\w+)\s*#\s*(.*)$')
        rangeValueRE = re.compile('([0-9A-F]+)\.\.([0-9A-F]+)\s*;\s*(\w+)\s*#\s*(.*)$')

        # collect
        cat_name = ''
        cats = dict()
        while True:
            line = f.readline()
            if not line:
                break
            m = headerRE.match(line)
            if m:
                cat_name = m.group(1)
                if not (cat_name in cats):
                    cats[cat_name] = []
            if len(line) == 0 or line[0] == '#':
                continue
            m = singleValueRE.match(line)
            if m:
                code = int(m.group(1), 16)
                prop = m.group(2) # ignored
                comment = m.group(3)
                cats[cat_name].append({'start': code, 'end': code, 'comment': comment})
            m = rangeValueRE.match(line)
            if m:
                start = int(m.group(1), 16)
                end = int(m.group(2), 16)
                prop = m.group(3) # ignored
                comment = m.group(4)
                cats[cat_name].append({'start': start, 'end': end, 'comment': comment})

        # write range tables
        impl.write("namespace tables {\n")
        for name in sorted(cats.keys()):
            impl.write("auto constexpr {} = std::array{{ // {{{{{{\n".format(name))
            for propRange in cats[name]:
                impl.write("    Interval{{ 0x{:>04X}, 0x{:>04X} }}, // {}\n".format(propRange['start'], propRange['end'], propRange['comment']))
            impl.write("}; // }}}\n")
        impl.write("} // end namespace tables\n\n")

        # write out test function
        impl.write("bool contains(General_Category _cat, char32_t _codepoint) noexcept {\n")
        impl.write("    switch (_cat) {\n")
        for name in sorted(cats.keys()):
            impl.write("        case General_Category::{0:}: return contains(tables::{0:}, _codepoint);\n".format(name))
        impl.write("    }\n")
        impl.write("    return false;\n")
        impl.write("}\n\n")

        # write enums / signature
        header.write("enum class General_Category {\n")
        for name in sorted(cats.keys()):
            header.write("    {},\n".format(name))
        header.write("};\n\n")
        header.write("bool contains(General_Category _cat, char32_t _codepoint) noexcept;\n\n")

def main():
    header = open(TARGET_HEADER_FILE, 'w')
    impl = open(TARGET_IMPL_FILE, 'w')

    file_header(header, impl)
    process_core_props(header, impl)
    process_derived_general_category(header, impl)
    process_grapheme_break_props(header, impl)
    process_emoji_props(header, impl)
    file_footer(header, impl)

main()
