/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019 Christian Parpart <christian@parpart.family>
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
#include <terminal/Util.h>
#include <terminal/util/UTF8.h>

namespace terminal {

std::string escape(char32_t ch)
{
    switch (ch)
    {
        case '\\':
            return "\\\\";
        case 0x1B:
            return "\\033";
        case '\t':
            return "\\t";
        case '\r':
            return "\\r";
        case '\n':
            return "\\n";
        case '"':
            return "\\\"";
        default:
            if (ch <= 0xFF && std::isprint(ch))
                return fmt::format("{}", static_cast<char>(ch));
            else if (ch <= 0xFF)
                return fmt::format("\\x{:02X}", static_cast<uint8_t>(ch));
            else
            {
                auto const bytes = utf8::encode(ch);
                auto res = std::string{};
                for (auto const byte : bytes)
                    res += byte; // fmt::format("\\x{:02X}", static_cast<unsigned>(byte));
                return res;
            }
    }
}

} // namespace terminal
