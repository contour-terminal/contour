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
DERIVED_GENERAL_CATEGORY_FILE = UCD_DIR + '/extracted/DerivedGeneralCategory.txt'

TARGET_HEADER_FILE = PROJECT_ROOT + '/src/crispy/text/Unicode.h'
TARGET_IMPL_FILE = PROJECT_ROOT + '/src/crispy/text/Unicode.cpp'

def file_header(header, impl):
    header.write(globals()['__doc__'])
    header.write("""#pragma once

#include <array>
#include <utility>

namespace crispy::text {

""")

    impl.write(globals()['__doc__'])
    impl.write("""
#include <crispy/text/Unicode.h>

#include <array>

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
        size_t a = 0;
        size_t b = _ranges.size() - 1;
        while (a < b)
        {
            size_t i = (b + a) / 2;
            auto const& I = _ranges[i];
            if (_codepoint < I.from)
                b = a + (b - a) / 2;
            else if (_codepoint > I.to)
                a = a + (b - a) / 2;
            else if (I.from <= _codepoint && _codepoint <= I.to)
                return true;
        }
        return false;
    }
}

""")

def file_footer(header, impl):
    header.write("} // end namespace\n")
    impl.write("} // end namespace\n")

def process_core_props(header, impl):
    with open(DERIVED_CORE_PROPERTIES_FILE, 'r') as f:
        singleValueRE = re.compile('([0-9A-F]+)\s+;\s*(\w+)\s+#\s*(.*)$')
        rangeValueRE = re.compile('([0-9A-F]+)\.\.([0-9A-F]+)\s+;\s*(\w+)\s+#\s*(.*)$')

        # collect
        coreProperties = dict()
        while True:
            line = f.readline()
            if not line:
                break
            if len(line) == 0 or line[0] == '#':
                continue
            m = singleValueRE.match(line)
            if m:
                code = m.group(1)
                prop = m.group(2)
                comment = m.group(3)
                if not (prop in coreProperties):
                    coreProperties[prop] = []
                coreProperties[prop].append({'start': code, 'end': code, 'comment': comment})
            m = rangeValueRE.match(line)
            if m:
                start = m.group(1)
                end = m.group(2)
                prop = m.group(3)
                comment = m.group(4)
                if not (prop in coreProperties):
                    coreProperties[prop] = []
                coreProperties[prop].append({'start': start, 'end': end, 'comment': comment})

        # write range tables
        impl.write("namespace tables {\n")
        for name in sorted(coreProperties.keys()):
            impl.write("auto constexpr {} = std::array{{ // {{{{{{\n".format(name))
            for propRange in coreProperties[name]:
                impl.write("    Interval{{ 0x{}, 0x{} }}, // {}\n".format(propRange['start'], propRange['end'], propRange['comment']))
            impl.write("}; // }}}}}}\n")
        impl.write("} // end namespace tables\n\n")

        # write out test function
        impl.write("bool contains(Core_Property _prop, char32_t _codepoint) noexcept {\n")
        impl.write("    switch (_prop) {\n")
        for name in sorted(coreProperties.keys()):
            impl.write("        case Core_Property::{0:}: return contains(tables::{0:}, _codepoint);\n".format(name))
        impl.write("    }\n")
        impl.write("    return false;\n")
        impl.write("}\n\n")

        # API: write enum and tester
        header.write("enum class Core_Property {\n")
        for name in sorted(coreProperties.keys()):
            header.write("    {},\n".format(name))
        header.write("};\n\n")
        header.write("bool contains(Core_Property _prop, char32_t _codepoint) noexcept;\n\n")

def process_derived_general_category(header, impl):
    with open(DERIVED_GENERAL_CATEGORY_FILE, 'r') as f:
        # General_Category=Spacing_Mark
        headerRE = re.compile('^#\s*General_Category=(\w+)$')
        singleValueRE = re.compile('([0-9A-F]+)\s+;\s*(\w+)\s+#\s*(.*)$')
        rangeValueRE = re.compile('([0-9A-F]+)\.\.([0-9A-F]+)\s+;\s*(\w+)\s+#\s*(.*)$')

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
                code = m.group(1)
                prop = m.group(2) # ignored
                comment = m.group(3)
                cats[cat_name].append({'start': code, 'end': code, 'comment': comment})
            m = rangeValueRE.match(line)
            if m:
                start = m.group(1)
                end = m.group(2)
                prop = m.group(3) # ignored
                comment = m.group(4)
                cats[cat_name].append({'start': start, 'end': end, 'comment': comment})

        # write range tables
        impl.write("namespace tables {\n")
        for name in sorted(cats.keys()):
            impl.write("auto constexpr {} = std::array{{ // {{{{{{\n".format(name))
            for propRange in cats[name]:
                impl.write("    Interval{{ 0x{}, 0x{} }}, // {}\n".format(propRange['start'], propRange['end'], propRange['comment']))
            impl.write("}; // }}}}}}\n")
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
    file_footer(header, impl)

main()
