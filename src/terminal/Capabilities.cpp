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
#include <terminal/Capabilities.h>

using std::string_view;
using std::string;

using namespace terminal::capabilities::literals;
using namespace std::string_view_literals;

namespace terminal::capabilities {

template<typename T>
struct Cap
{
    Code code;
    T value;
};

using Boolean = Cap<bool>;
using Numeric = Cap<int>;
using String = Cap<std::string_view>;

namespace {
    constexpr inline auto booleanCaps = std::array{
        Boolean{ "am"_tcap, true },  // terminal has automatic margins
        Boolean{ "ut"_tcap, true },  // screen erased with background color
        Boolean{ "cc"_tcap, true },  // terminal can re-define existing colors
        Boolean{ "xn"_tcap, true },  // newline ignored after 80 cols (concept)
        Boolean{ "km"_tcap, true },  // Has a meta key (i.e., sets 8th-bit)
        Boolean{ "mi"_tcap, true },  // safe to move while in insert mode
        Boolean{ "ms"_tcap, true },  // safe to move while in standout mode
        Boolean{ "NP"_tcap, true },  // pad character does not exist
        Boolean{ "5i"_tcap, true },  // printer will not echo on screen
        Boolean{ "YD"_tcap, true },  // only positive motion for vpa/mvpa caps
        Boolean{ "Tc"_tcap, true },  // RGB color support (introduced by Tmux in 2016)
    };

    constexpr inline auto numericalCaps = std::array{
        Numeric{ "co"_tcap, 80 },   // number of columns in a line
        Numeric{ "it"_tcap, 8 },    // tabs initially every # spaces
        Numeric{ "Co"_tcap, 256 },  // maximum number of colors on screen
        Numeric{ "pa"_tcap, 65536 },// maximum number of color-pairs on the screen
    };

    constexpr inline auto stringCaps = std::array{
        String{ "ac"_tcap, "``aaffggiijjkkllmmnnooppqqrrssttuuvvwwxxyyzz{{||}}~~"sv }, // graphics charset pairs, based on vt100
        String{ "bl"_tcap, "^G"sv }, // The audible bell character
        String{ "md"_tcap, "\E[1m"sv }, // Escape code for bold
        String{ "bt"_tcap, "\E[Z"sv }, // Back tab
        String{ "kB"_tcap, "\E[Z"sv },
        String{ "vi"_tcap, "\E[?25l"sv }, // Make cursor invisible
        String{ "cl"_tcap, "\E[H\E[2J"sv }, // Clear screen
        String{ "ve"_tcap, "\E[?12l\E[?25h"sv }, // Make cursor appear normal
        String{ "cr"_tcap, "^M"sv },  // CR (carriage return \r)
        String{ "cs"_tcap, "\E[%i%p1%d;%p2%dr"sv }, // Change scroll region
        String{ "LE"_tcap, "\E[%p1%dD"sv }, // // Move cursor to the left by the specified amount
        String{ "le"_tcap, "^H"sv },  // BS (backspace)
        // Move cursor down specified number of lines
        String{ "DO"_tcap, "\E[%p1%dB"sv },
        String{ "do"_tcap, "^J"sv },  // LF (line-feed \n)
        // Move cursor to the right by the specified amount
        String{ "RI"_tcap, "\E[%p1%dC"sv },
        String{ "nd"_tcap, "\E[C"sv },
        // Move cursor up specified number of lines
        String{ "UP"_tcap, "\E[%p1%dA"sv },
        String{ "up"_tcap, "\E[A"sv },
        // Move cursor to specified location
        String{ "cm"_tcap, "\E[%i%p1%d;%p2%dH"sv },
        // Make cursor very visible
        String{ "vs"_tcap, "\E[?12;25h"sv },
        // Delete the specified number of characters
        String{ "DC"_tcap, "\E[%p1%dP"sv },
        String{ "dc"_tcap, "\E[P"sv },
        // Turn on half bright mode
        String{ "mh"_tcap, "\E[2m"sv },
        // Delete the specified number of lines
        String{ "DL"_tcap, "\E[%p1%dM"sv },
        String{ "dl"_tcap, "\E[M"sv },
        // Erase specified number of characters
        String{ "ec"_tcap, "\E[%p1%dX"sv },
        // Clear to end of screen
        String{ "cd"_tcap, "\E[J"sv },
        String{ "ce"_tcap, "\E[K"sv }, // Clear to end of line
        String{ "cb"_tcap, "\E[1K"sv }, // Clear to start of line
        String{ "vb"_tcap, "\E[?5h$<100/>\E[?5l"sv },   // visible bell
        String{ "ho"_tcap, "\E[H"sv },                  // Home cursor
        String{ "ch"_tcap, "\E[%i%p1%dG"sv },           // Move cursor to column
        String{ "ht"_tcap, "^I"sv },                    // Move to next tab
        String{ "st"_tcap, "\EH"sv },                   // Set tabstop at current position
        String{ "IC"_tcap, "\E[%p1%d@"sv },             // Insert specified number of characters
        String{ "AL"_tcap, "\E[%p1%dL"sv },             // insert #1 lines (P*)
        String{ "al"_tcap, "\E[L"sv },                  // insert line (P*)
        String{ "sf"_tcap, "^J"sv },                    // scroll up by specified amount
        String{ "SF"_tcap, "\E[%p1%dS"sv },             // scroll forward #1 lines (P)
        // initialize color (set dynamic colors)
        String{ "Ic"_tcap, "\E]4;%p1%d;rgb:%p2%{255}%*%{1000}%/%2.2X/%p3%{255}%*%{1000}%/%2.2X/%p4%{255}%*%{1000}%/%2.2X\E\\"sv },
        // Set all colors to original values
        String{ "oc"_tcap, "\E]104\007"sv },
        // turn on blank mode (characters invisible)
        String{ "mk"_tcap, "\E[8m"sv },     // turn on blank mode (characters invisible)
        String{ "kb"_tcap, "\177"sv }, // Backspace
        String{ "Km"_tcap, "\E[M"sv }, // Mouse event has occurred
        String{ "kR"_tcap, "\E[1;2A"sv }, // Scroll backwards (reverse index)
        String{ "kF"_tcap, "\E[1;2B"sv }, // scroll forwards (index)
        String{ "rc"_tcap, "\E8"sv }, // Restore cursor
        String{ "rp"_tcap, "%p1%c\E[%p2%{1}%-%db"sv }, // Repeat preceding character
        String{ "mr"_tcap, "\E[7m"sv }, // Reverse video
        String{ "sr"_tcap, "\EM"sv }, // Scroll backwards the specified number of lines (reverse index)
        String{ "SR"_tcap, "\E[%p1%dT"sv },
        String{ "RA"_tcap, "\E[?7l"sv }, // Turn off automatic margins
        String{ "te"_tcap, "\E[?1049l"sv }, // Exit alternate screen
        String{ "ei"_tcap, "\E[4l"sv }, // Exit insert mode
        String{ "ke"_tcap, "\E[?1l"sv }, // Exit application keypad mode
        String{ "se"_tcap, "\E[27m"sv }, // Exit standout mode
        String{ "ue"_tcap, "\E[24m"sv }, // Exit underline mode
        String{ "Te"_tcap, "\E[29m"sv }, // Exit strikethrough mode
        String{ "r1"_tcap, "\E]\E\\\Ec"sv }, // Reset string1 (empty OSC sequence to exit OSC/OTH modes, and regular reset)
        String{ "sc"_tcap, "\E7"sv }, // Save cursor
        String{ "AB"_tcap, "\E[%?%p1%{8}%<%t4%p1%d%e%p1%{16}%<%t10%p1%{8}%-%d%e48;5;%p1%d%;m"sv }, // Set background color
        String{ "AF"_tcap, "\E[%?%p1%{8}%<%t3%p1%d%e%p1%{16}%<%t9%p1%{8}%-%d%e38;5;%p1%d%;m"sv }, // Set foreground color
        String{ "sa"_tcap, "%?%p9%t\E(0%e\E(B%;\E[0%?%p6%t;1%;%?%p2%t;4%;%?%p1%p3%|%t;7%;%?%p4%t;5%;%?%p7%t;8%;m"sv }, // Set attributes
        String{ "me"_tcap, "\E(B\E[m"sv },     // Clear all attributes
        String{ "op"_tcap, "\E[39;49m"sv },   // Reset color pair to its original value
        String{ "SA"_tcap, "\E[?7h"sv },      // Turn on automatic margins
        String{ "ti"_tcap, "\E[?1049h"sv },   // Start alternate screen
        String{ "im"_tcap, "\E[4h"sv },       // Enter insert mode
        String{ "ks"_tcap, "\E[?1h"sv },      // Enter application keymap mode
        String{ "so"_tcap, "\E[7m"sv },       // Enter standout mode
        String{ "us"_tcap, "\E[4m"sv },       // Enter underline mode
        String{ "Ts"_tcap, "\E[9m"sv },       // Enter strikethrough mode
        String{ "ct"_tcap, "\E[3g"sv },       // Clear all tab stops
        String{ "ts"_tcap, "\E]2;"sv },       // To status line (used to set window titles)
        String{ "fs"_tcap, "^G"sv },          // From status line (end window title string)
        String{ "ds"_tcap, "\E]2;\007"sv },   // Disable status line (clear window title)
        String{ "cv"_tcap, "\E[%i%p1%dd"sv }, // Move to specified line
        String{ "ZH"_tcap, "\E[3m"sv },       // Enter italics mode
        String{ "ZR"_tcap, "\E[23m"sv },      // Leave italics mode
        String{ "as"_tcap, "\E(0"sv }, // start alternate character set (P)
        String{ "ae"_tcap, "\E(B"sv }, // end alternate character set (P)

        // non-standard: used by NeoVIM
        // String{ "setrgbf"_tcap, "\E[38:2:%p1%d:%p2%d:%p3%dm"sv }, // setrgbf: Set RGB foreground color
        // String{ "setrgbb"_tcap, "\E[48:2:%p1%d:%p2%d:%p3%dm"sv }, // setrgbb: Set RGB background color
    };
}

bool StaticDatabase::booleanCapability(Code _cap) const
{
    for (auto const& cap: booleanCaps)
        if (cap.code.code == _cap.code)
            return cap.value;

    return false;
}

int StaticDatabase::numericCapability(Code _cap) const
{
    for (auto const& cap: numericalCaps)
        if (cap.code.code == _cap.code)
            return cap.value;

    return -1;
}

string_view StaticDatabase::stringCapability(Code _cap) const
{
    for (auto const& cap: stringCaps)
        if (cap.code.code == _cap.code)
            return cap.value;

    return {};
}

string StaticDatabase::terminfo() const
{
    return ""; // TODO
}

}
