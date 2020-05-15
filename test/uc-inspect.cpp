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
#include <crispy/text/wcwidth.h>
#include <crispy/text/Unicode.h>
#include <crispy/UTF8.h>
#include <crispy/escape.h>
#include <crispy/overloaded.h>
#include <iomanip>
#include <fstream>

using namespace std;
using namespace crispy;

bool isEmoji(char32_t ch)
{
    return text::emoji(ch) && !text::emoji_component(ch);
}

// TODO
// void grapheme_clusters(istream& _in)
// {
//     while (_in.good())
//     {
//     }
// }

void codepoints(istream& _in)
{
    auto constexpr mbmax = 5;
    char mb[mbmax] = {0};
    size_t mblen = 0;
    size_t lastOffset = 0;
    size_t totalOffset = 0;

    for (; _in.good() && totalOffset < 50;)
    {
        char ch;
        _in.read(&ch, sizeof(char));
        totalOffset++;
        if (mblen < mbmax)
        {
            mb[mblen++] = ch;
            char32_t wc = 0;
            if (crispy::text::mbtowc(&wc, mb, mblen) != -1)
            {
                int const width = crispy::text::wcwidth(wc);
                if (width >= 0)
                {
                    auto u8 = crispy::utf8::encode(wc);
                    printf("%3lu: mblen:%lu width:%d [%5s] [%5s] U+%08x UTF8:%s\n",
                            lastOffset,
                            mblen,
                            width,
                            isEmoji(wc) ? "EMOJI" : "TEXT",
                            "LATIN", // TODO: script
                            wc,
                            crispy::escape(u8.begin(), u8.end()).c_str()
                    );
                }
                lastOffset = totalOffset;
                mblen = 0;
            }
        }
        else
        {
            mblen = 0;
            mb[mblen++] = ch;
            //cerr << "mblen would exceed. Resetting. Invalid utf-8?\n";
        }
    }
}

int main([[maybe_unused]] int argc, char const* argv[])
{
    // TODO: hb-inspect --codepoints FILE           Inspects source by UTF-32 codepoints
    // TODO: hb-inspect --grapheme-clusters FILE    Inspects source by grapheme cluster
    // TODO: hb-inspect --script-clusters FILE      Inspects source by script cluster

    auto in = ifstream(argv[1], ios::binary);

    codepoints(in);

    return EXIT_SUCCESS;
}
