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
EAST_ASIAN_WIDTH = UCD_DIR + '/EastAsianWidth.txt'

class UCDGenerator:
    def __init__(self, _ucd_dir, _header_file, _impl_file):
        self.ucd_dir = _ucd_dir
        self.header = open(_header_file, 'w')
        self.impl = open(_impl_file, 'w')
        self.singleValueRE = re.compile('([0-9A-F]+)\s*;\s*(\w+)\s*#\s*(.*)$')
        self.rangeValueRE = re.compile('([0-9A-F]+)\.\.([0-9A-F]+)\s*;\s*(\w+)\s*#\s*(.*)$')

    def close(self):
        self.header.close()
        self.impl.close()

    def generate(self):
        self.file_header()
        self.process_core_props()
        self.process_derived_general_category()
        self.process_grapheme_break_props()
        self.process_east_asian_width()
        self.process_emoji_props()
        self.file_footer()
        return

    def file_header(self): # {{{
        self.header.write(globals()['__doc__'])
        self.header.write("""#pragma once

#include <array>
#include <optional>
#include <utility>

namespace crispy::text {

""")

        self.impl.write(globals()['__doc__'])
        self.impl.write("""
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
            auto const i = (b + a) / 2;
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
            auto const i = (b + a) / 2;
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

""") # }}}

    def file_footer(self): # {{{
        self.header.write("} // end namespace\n")
        self.impl.write("} // end namespace\n")
# }}}

    def process_core_props(self):
        with open(DERIVED_CORE_PROPERTIES_FILE, 'r') as f:
            # collect
            props = dict()
            while True:
                line = f.readline()
                if not line:
                    break
                if len(line) == 0 or line[0] == '#':
                    continue
                m = self.singleValueRE.match(line)
                if m:
                    code = int(m.group(1), 16)
                    prop = m.group(2)
                    comment = m.group(3)
                    if not (prop in props):
                        props[prop] = []
                    props[prop].append({'start': code, 'end': code, 'comment': comment})
                m = self.rangeValueRE.match(line)
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
            self.impl.write("namespace tables {\n")
            for name in sorted(props.keys()):
                self.impl.write("auto constexpr {} = std::array{{ // {{{{{{\n".format(name))
                for propRange in props[name]:
                    self.impl.write("    Interval{{ 0x{:>04X}, 0x{:>04X} }}, // {}\n".format(propRange['start'], propRange['end'], propRange['comment']))
                self.impl.write("}; // }}}\n")
            self.impl.write("} // end namespace tables\n\n")

            # write out test function
            self.impl.write("bool contains(Core_Property _prop, char32_t _codepoint) noexcept {\n")
            self.impl.write("    switch (_prop) {\n")
            for name in sorted(props.keys()):
                self.impl.write("        case Core_Property::{0:}: return contains(tables::{0:}, _codepoint);\n".format(name))
            self.impl.write("    }\n")
            self.impl.write("    return false;\n")
            self.impl.write("}\n\n")

            # API: write enum and tester
            self.header.write("enum class Core_Property {\n")
            for name in sorted(props.keys()):
                self.header.write("    {},\n".format(name))
            self.header.write("};\n\n")
            self.header.write("bool contains(Core_Property _prop, char32_t _codepoint) noexcept;\n\n")

    def process_props(self, filename, prop_key):
        with open(filename, 'r') as f:
            headerRE = re.compile('^#\s*{}:\s*(\w+)$'.format(prop_key))

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
                m = self.singleValueRE.match(line)
                if m:
                    code = int(m.group(1), 16)
                    prop = m.group(2)
                    comment = m.group(3)
                    props[props_name].append({'start': code, 'end': code, 'property': prop, 'comment': comment})
                m = self.rangeValueRE.match(line)
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
            self.impl.write("namespace tables {\n")
            for name in sorted(props.keys()):
                self.impl.write("auto constexpr {} = std::array{{ // {{{{{{\n".format(name))
                for propRange in props[name]:
                    self.impl.write("    Prop<crispy::text::{}>{{ {{ 0x{:>04X}, 0x{:>04X} }}, crispy::text::{}::{} }}, // {}\n".format(
                               name,
                               propRange['start'],
                               propRange['end'],
                               name,
                               propRange['property'],
                               propRange['comment']))
                self.impl.write("}; // }}}\n")
            self.impl.write("} // end namespace tables\n\n")

            # write enums / signature
            for name in sorted(props.keys()):
                self.header.write('enum class {} {{\n'.format(name))
                enums = set()
                for enum in props[name]:
                    enums.add(enum['property'])
                for enum in sorted(enums):
                    self.header.write("    {},\n".format(enum))
                self.header.write("};\n\n")

            for name in sorted(props.keys()):
                self.impl.write('namespace {} {{\n'.format(name.lower()))
                self.header.write('namespace {} {{\n'.format(name.lower()))
                enums = set()
                for enum in props[name]:
                    enums.add(enum['property'])
                for enum in sorted(enums):
                    self.header.write('    bool {}(char32_t _codepoint) noexcept;\n'.format(enum.lower()))
                    self.impl.write('    bool {}(char32_t _codepoint) noexcept {{\n'.format(enum.lower()))
                    self.impl.write("        if (auto p = search(tables::{}, _codepoint); p.has_value())\n".format(name))
                    self.impl.write('            return p.value() == {}::{};\n'.format(name, enum))
                    self.impl.write('        return false;\n')
                    self.impl.write('    }\n\n')
                self.header.write('}\n')
                self.impl.write('}\n')
            self.header.write('\n')
            self.impl.write('\n')

    def process_grapheme_break_props(self):
        self.process_props(GRAPHEME_BREAK_PROPS_FILE, 'Property')

    def parse_range(self, line):
        m = self.singleValueRE.match(line)
        if m:
            code = int(m.group(1), 16)
            prop = m.group(2)
            comment = m.group(3)
            return {'start': code, 'end': code, 'property': prop, 'comment': comment}
        m = self.rangeValueRE.match(line)
        if m:
            start = int(m.group(1), 16)
            end = int(m.group(2), 16)
            prop = m.group(3)
            comment = m.group(4)
            return {'start': start, 'end': end, 'property': prop, 'comment': comment}
        return None

    def process_emoji_props(self):
        with open(EXTENDED_PICTOGRAPHIC_FILE, 'r') as f:
            # collect
            props_name = ''
            props = dict()
            while True:
                line = f.readline()
                if not line:
                    break
                r = self.parse_range(line)
                if r != None:
                    name = r['property']
                    if not name in props:
                        props[name] = []
                    props[name].append(r)

            # sort table
            for prop_key in props.keys():
                    props[prop_key].sort(key = lambda a: a['start'])

            # write range tables
            self.impl.write("namespace tables {\n")
            for name in sorted(props.keys()):
                self.impl.write("auto constexpr {} = std::array{{ // {{{{{{\n".format(name))
                for propRange in props[name]:
                    self.impl.write("    Interval{{ 0x{:>04X}, 0x{:>04X} }}, // {}\n".format(
                               propRange['start'],
                               propRange['end'],
                               propRange['comment']))
                self.impl.write("}; // }}}\n")
            self.impl.write("} // end namespace tables\n\n")

            # write out test function
            for name in sorted(props.keys()):
                self.impl.write('bool {}(char32_t _codepoint) noexcept {{\n'.format(name.lower()))
                self.impl.write("    return contains(tables::{0:}, _codepoint);\n".format(name))
                self.impl.write("}\n\n")

            # write enums / signature
            for name in sorted(props.keys()):
                self.header.write('bool {}(char32_t _codepoint) noexcept;\n'.format(name.lower()))
            self.header.write('\n')

    def process_derived_general_category(self):
        with open(DERIVED_GENERAL_CATEGORY_FILE, 'r') as f:
            # General_Category=Spacing_Mark
            headerRE = re.compile('^#\s*General_Category=(\w+)$')
            self.rangeValueRE = re.compile('([0-9A-F]+)\.\.([0-9A-F]+)\s*;\s*(\w+)\s*#\s*(.*)$')

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
                m = self.singleValueRE.match(line)
                if m:
                    code = int(m.group(1), 16)
                    prop = m.group(2) # ignored
                    comment = m.group(3)
                    cats[cat_name].append({'start': code, 'end': code, 'comment': comment})
                m = self.rangeValueRE.match(line)
                if m:
                    start = int(m.group(1), 16)
                    end = int(m.group(2), 16)
                    prop = m.group(3) # ignored
                    comment = m.group(4)
                    cats[cat_name].append({'start': start, 'end': end, 'comment': comment})

            # write range tables
            self.impl.write("namespace tables {\n")
            for name in sorted(cats.keys()):
                self.impl.write("auto constexpr {} = std::array{{ // {{{{{{\n".format(name))
                for propRange in cats[name]:
                    self.impl.write("    Interval{{ 0x{:>04X}, 0x{:>04X} }}, // {}\n".format(propRange['start'], propRange['end'], propRange['comment']))
                self.impl.write("}; // }}}\n")
            self.impl.write("} // end namespace tables\n\n")

            # write out test function
            self.impl.write("bool contains(General_Category _cat, char32_t _codepoint) noexcept {\n")
            self.impl.write("    switch (_cat) {\n")
            for name in sorted(cats.keys()):
                self.impl.write("        case General_Category::{0:}: return contains(tables::{0:}, _codepoint);\n".format(name))
            self.impl.write("    }\n")
            self.impl.write("    return false;\n")
            self.impl.write("}\n\n")

            # write enums / signature
            self.header.write("enum class General_Category {\n")
            for name in sorted(cats.keys()):
                self.header.write("    {},\n".format(name))
            self.header.write("};\n\n")

            self.header.write("bool contains(General_Category _cat, char32_t _codepoint) noexcept;\n\n")

            self.header.write('namespace general_category {\n')
            for name in sorted(cats.keys()):
                self.header.write(
                        '    inline bool {}(char32_t _codepoint) {{ return contains(General_Category::{}, _codepoint); }}\n'.
                        format(name.lower(), name))
            self.header.write('}\n\n')

    def collect_range_table_with_prop(self, f):
        table = []
        while True:
            line = f.readline()
            if not line:
                break
            m = self.singleValueRE.match(line)
            if m:
                code = int(m.group(1), 16)
                prop = m.group(2)
                comment = m.group(3)
                table.append({'start': code, 'end': code, 'property': prop, 'comment': comment})
            m = self.rangeValueRE.match(line)
            if m:
                start = int(m.group(1), 16)
                end = int(m.group(2), 16)
                prop = m.group(3)
                comment = m.group(4)
                table.append({'start': start, 'end': end, 'property': prop, 'comment': comment})
        table.sort(key = lambda a: a['start'])
        return table

    def process_east_asian_width(self):
        WIDTH_NAMES = {
            'A': "Ambiguous",
            'F': "FullWidth",
            'H': 'HalfWidth',
            'N': 'Neutral',
            'Na': 'Narrow',
            'W': "Wide",
        }
        type_name = 'EastAsianWidth'
        table_name = type_name
        prop_type = 'crispy::text::{}'.format(type_name)

        with open(self.ucd_dir + '/EastAsianWidth.txt') as f:
            table = self.collect_range_table_with_prop(f)

            # api: enum
            self.header.write('enum class {} {{\n'.format(table_name))
            for v in WIDTH_NAMES.values():
                self.header.write('    {},\n'.format(v))
            self.header.write('};\n\n')

            # api: signature
            self.header.write('EastAsianWidth east_asian_width(char32_t _codepoint) noexcept;\n\n')

            # impl: range tables
            self.impl.write("namespace tables {\n")
            self.impl.write("auto constexpr {} = std::array{{ // {{{{{{\n".format(table_name))
            for propRange in table:
                self.impl.write("    Prop<{}>{{ {{ 0x{:>04X}, 0x{:>04X} }}, {}::{} }}, // {}\n".format(
                                prop_type,
                                propRange['start'],
                                propRange['end'],
                                prop_type,
                                WIDTH_NAMES[propRange['property']],
                                propRange['comment']))
            self.impl.write("}; // }}}\n")
            self.impl.write("} // end namespace tables\n\n")

            # impl: function
            self.impl.write(
                'EastAsianWidth east_asian_width(char32_t _codepoint) noexcept {\n' +
                '    if (auto const p = search(tables::EastAsianWidth, _codepoint); p.has_value())\n'
                '        return p.value();\n' +
                '    return EastAsianWidth::Neutral;\n' + # XXX default
                '}\n'
            )

        return

def main():
    ucdgen = UCDGenerator(UCD_DIR,
                          PROJECT_ROOT + '/src/crispy/text/Unicode.h',
                          PROJECT_ROOT + '/src/crispy/text/Unicode.cpp')
    ucdgen.generate()

main()
