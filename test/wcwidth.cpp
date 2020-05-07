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
#include <crispy/UTF8.h>
#include <crispy/overloaded.h>
#include <iomanip>
#include <fstream>

using namespace std;

int main([[maybe_unused]] int argc, char const* argv[])
{
    auto in = ifstream(argv[1], ios::binary);

    auto constexpr mbmax = 5;
    char mb[mbmax] = {0};
    size_t mblen = 0;
    size_t totalOffset = 0;

    for (; in.good() && totalOffset < 50;)
    {
        char ch;
        in.read(&ch, sizeof(char));
        totalOffset++;
        if (mblen < mbmax)
        {
            mb[mblen++] = ch;
            //printf("%lu: %lu ch: 0x%02x\n", totalOffset, mblen, ch & 0xFF);
            char32_t wc = 0;
            if (crispy::utf8::mbtowc(&wc, mb, mblen) != -1)
            {
                int const width = crispy::utf8::wcwidth(wc);
                if (width >= 0)
                {
                    printf("%3lu: mblen:%lu, wc:0x%08x, len:%d\n",
                            totalOffset,
                            mblen,
                            wc,
                            width);
                }
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

    return EXIT_SUCCESS;
}
