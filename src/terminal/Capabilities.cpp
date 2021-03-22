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
    std::string_view name;
    T value;
};

using Boolean = Cap<bool>;
using Numeric = Cap<int>;
using String = Cap<std::string_view>;

namespace {
    constexpr inline auto booleanCaps = std::array{
        Boolean{ "am"_tcap, "sm"sv, true },  // terminal has automatic margins
        Boolean{ "ut"_tcap, "bce"sv, true },  // screen erased with background color
        Boolean{ "cc"_tcap, "ccc"sv, true },  // terminal can re-define existing colors
        Boolean{ "xn"_tcap, "xenl"sv, true },  // newline ignored after 80 cols (concept)
        Boolean{ "km"_tcap, "km"sv, true },  // Has a meta key (i.e., sets 8th-bit)
        Boolean{ "mi"_tcap, "mir"sv, true },  // safe to move while in insert mode
        Boolean{ "ms"_tcap, "msgr"sv, true },  // safe to move while in standout mode
        Boolean{ "NP"_tcap, "npc"sv, true },  // pad character does not exist
        Boolean{ "5i"_tcap, "mc5i"sv, true },  // printer will not echo on screen
        Boolean{ "YD"_tcap, "xvpa"sv, true },  // only positive motion for vpa/mvpa caps
        // NB: The tcap name "truecol" is made up. I didn't find a well accepted one.
        Boolean{ "Tc"_tcap, "truecol"sv, true },  // RGB color support (introduced by Tmux in 2016)
    };

    constexpr inline auto numericalCaps = std::array{
        Numeric{ "co"_tcap, "cols"sv, 80 },   // number of columns in a line
        Numeric{ "it"_tcap, "it"sv, 8 },    // tabs initially every # spaces
        Numeric{ "Co"_tcap, "colors"sv, 256 },  // maximum number of colors on screen
        Numeric{ "pa"_tcap, "pairs"sv, 65536 },// maximum number of color-pairs on the screen
    };

    constexpr inline auto stringCaps = std::array{
        String{ "TN"_tcap, ""sv, "xterm-256color"sv }, // termcap/terminfo name (xterm extension)
        String{ "ac"_tcap, "acsc"sv, "``aaffggiijjkkllmmnnooppqqrrssttuuvvwwxxyyzz{{||}}~~"sv }, // graphics charset pairs, based on vt100
        String{ "bl"_tcap, "bel"sv, "^G"sv }, // The audible bell character
        String{ "md"_tcap, "bold"sv, "\E[1m"sv }, // Escape code for bold
        String{ "bt"_tcap, "cbt"sv, "\E[Z"sv }, // Back tab
        String{ "kB"_tcap, "kcbt"sv, "\E[Z"sv },
        String{ "vi"_tcap, "civis"sv, "\E[?25l"sv }, // Make cursor invisible
        String{ "cl"_tcap, "clear"sv, "\E[H\E[2J"sv }, // Clear screen
        String{ "ve"_tcap, "cnorm"sv, "\E[?12l\E[?25h"sv }, // Make cursor appear normal
        String{ "cr"_tcap, "cr"sv, "^M"sv },  // CR (carriage return \r)
        String{ "cs"_tcap, "csr"sv, "\E[%i%p1%d;%p2%dr"sv }, // Change scroll region
        String{ "LE"_tcap, "cub"sv, "\E[%p1%dD"sv }, // // Move cursor to the left by the specified amount
        String{ "le"_tcap, "cub1"sv, "^H"sv },  // BS (backspace)
        // Move cursor down specified number of lines
        String{ "DO"_tcap, "cud"sv, "\E[%p1%dB"sv },
        String{ "do"_tcap, "cud1"sv, "^J"sv },  // LF (line-feed \n)
        // Move cursor to the right by the specified amount
        String{ "RI"_tcap, "cuf"sv, "\E[%p1%dC"sv },
        String{ "nd"_tcap, "cuf1"sv, "\E[C"sv },
        // Move cursor up specified number of lines
        String{ "UP"_tcap, "cuu"sv,"" "\E[%p1%dA"sv },
        String{ "up"_tcap, "cuu1"sv,"" "\E[A"sv },
        // Move cursor to specified location
        String{ "cm"_tcap, "cup"sv, "\E[%i%p1%d;%p2%dH"sv },
        // Make cursor very visible
        String{ "vs"_tcap, "cvvis"sv, "\E[?12;25h"sv },
        // Delete the specified number of characters
        String{ "DC"_tcap, "dch"sv, "\E[%p1%dP"sv },
        String{ "dc"_tcap, "dch1"sv, "\E[P"sv },
        // Turn on half bright mode
        String{ "mh"_tcap, "dim"sv,"" "\E[2m"sv },
        // Delete the specified number of lines
        String{ "DL"_tcap, "dl"sv, "\E[%p1%dM"sv },
        String{ "dl"_tcap, "dl1"sv,"" "\E[M"sv },
        // Erase specified number of characters
        String{ "ec"_tcap, "ech"sv, "\E[%p1%dX"sv },
        // Clear to end of screen
        String{ "cd"_tcap, "ed"sv, "\E[J"sv },
        String{ "ce"_tcap, "el"sv, "\E[K"sv }, // Clear to end of line
        String{ "cb"_tcap, "el1"sv, "\E[1K"sv }, // Clear to start of line
        String{ "vb"_tcap, "flash"sv, "\E[?5h$<100/>\E[?5l"sv },   // visible bell
        String{ "ho"_tcap, "home"sv, "\E[H"sv },                  // Home cursor
        String{ "ch"_tcap, "hpa"sv, "\E[%i%p1%dG"sv },           // Move cursor to column
        String{ "ht"_tcap, "ht"sv, "^I"sv },                    // Move to next tab
        String{ "st"_tcap, "hts"sv, "\EH"sv },                   // Set tabstop at current position
        String{ "IC"_tcap, "ich"sv, "\E[%p1%d@"sv },             // Insert specified number of characters
        String{ "AL"_tcap, "il"sv, "\E[%p1%dL"sv },             // insert #1 lines (P*)
        String{ "al"_tcap, "il1"sv, "\E[L"sv },                  // insert line (P*)
        String{ "sf"_tcap, "ind"sv, "^J"sv },                    // scroll up by specified amount
        String{ "SF"_tcap, "indn"sv,"" "\E[%p1%dS"sv },             // scroll forward #1 lines (P)
        // initialize color (set dynamic colors)
        String{ "Ic"_tcap, "initc"sv, "\E]4;%p1%d;rgb:%p2%{255}%*%{1000}%/%2.2X/%p3%{255}%*%{1000}%/%2.2X/%p4%{255}%*%{1000}%/%2.2X\E\\"sv },
        // Set all colors to original values
        String{ "oc"_tcap, "oc"sv, "\E]104\007"sv },
        // turn on blank mode (characters invisible)
        String{ "mk"_tcap, "invis"sv, "\E[8m"sv },     // turn on blank mode (characters invisible)
        String{ "kb"_tcap, "kbs"sv, "\177"sv }, // Backspace
        String{ "Km"_tcap, "kmous"sv, "\E[M"sv }, // Mouse event has occurred
        String{ "kR"_tcap, "kri"sv, "\E[1;2A"sv }, // Scroll backwards (reverse index)
        String{ "kF"_tcap, "kind"sv, "\E[1;2B"sv }, // scroll forwards (index)
        String{ "rc"_tcap, "rc"sv, "\E8"sv }, // Restore cursor
        String{ "rp"_tcap, "rep"sv, "%p1%c\E[%p2%{1}%-%db"sv }, // Repeat preceding character
        String{ "mr"_tcap, "rev"sv,"" "\E[7m"sv }, // Reverse video
        String{ "sr"_tcap, "ri"sv, "\EM"sv }, // Scroll backwards the specified number of lines (reverse index)
        String{ "SR"_tcap, "rin"sv, "\E[%p1%dT"sv },
        String{ "RA"_tcap, "rmam"sv, "\E[?7l"sv }, // Turn off automatic margins
        String{ "te"_tcap, "rmcup"sv,"" "\E[?1049l"sv }, // Exit alternate screen
        String{ "ei"_tcap, "rmir"sv, "\E[4l"sv }, // Exit insert mode
        String{ "ke"_tcap, "rmkx"sv, "\E[?1l"sv }, // Exit application keypad mode
        String{ "se"_tcap, "rmso"sv, "\E[27m"sv }, // Exit standout mode
        String{ "ue"_tcap, "rmul"sv, "\E[24m"sv }, // Exit underline mode
        String{ "Te"_tcap, "rmxx"sv, "\E[29m"sv }, // Exit strikethrough mode
        String{ "r1"_tcap, "rs1"sv,"" "\E]\E\\\Ec"sv }, // Reset string1 (empty OSC sequence to exit OSC/OTH modes, and regular reset)
        String{ "sc"_tcap, "sc"sv, "\E7"sv }, // Save cursor
        String{ "AB"_tcap, "setab"sv, "\E[%?%p1%{8}%<%t4%p1%d%e%p1%{16}%<%t10%p1%{8}%-%d%e48;5;%p1%d%;m"sv }, // Set background color
        String{ "AF"_tcap, "setaf"sv, "\E[%?%p1%{8}%<%t3%p1%d%e%p1%{16}%<%t9%p1%{8}%-%d%e38;5;%p1%d%;m"sv }, // Set foreground color
        String{ "sa"_tcap, "sgr"sv, "%?%p9%t\E(0%e\E(B%;\E[0%?%p6%t;1%;%?%p2%t;4%;%?%p1%p3%|%t;7%;%?%p4%t;5%;%?%p7%t;8%;m"sv }, // Set attributes
        String{ "me"_tcap, "sgr0"sv, "\E(B\E[m"sv },     // Clear all attributes
        String{ "op"_tcap, "op"sv, "\E[39;49m"sv },   // Reset color pair to its original value
        String{ "SA"_tcap, "smam"sv,"" "\E[?7h"sv },      // Turn on automatic margins
        String{ "ti"_tcap, "smcup"sv, "\E[?1049h"sv },   // Start alternate screen
        String{ "im"_tcap, "smir"sv,"" "\E[4h"sv },       // Enter insert mode
        String{ "ks"_tcap, "smkx"sv, "\E[?1h"sv },      // Enter application keymap mode
        String{ "so"_tcap, "smso"sv, "\E[7m"sv },       // Enter standout mode
        String{ "us"_tcap, "smul"sv, "\E[4m"sv },       // Enter underline mode
        String{ "Ts"_tcap, "smxx"sv, "\E[9m"sv },       // Enter strikethrough mode
        String{ "ct"_tcap, "tbc"sv, "\E[3g"sv },       // Clear all tab stops
        String{ "ts"_tcap, "tsl"sv, "\E]2;"sv },       // To status line (used to set window titles)
        String{ "fs"_tcap, "fsl"sv, "^G"sv },          // From status line (end window title string)
        String{ "ds"_tcap, "dsl"sv,"" "\E]2;\007"sv },   // Disable status line (clear window title)
        String{ "cv"_tcap, "vpa"sv,"" "\E[%i%p1%dd"sv }, // Move to specified line
        String{ "ZH"_tcap, "sitm"sv, "\E[3m"sv },       // Enter italics mode
        String{ "ZR"_tcap, "ritm"sv,"" "\E[23m"sv },      // Leave italics mode
        String{ "as"_tcap, "smacs"sv,"" "\E(0"sv }, // start alternate character set (P)
        String{ "ae"_tcap, "rmacs"sv,"" "\E(B"sv }, // end alternate character set (P)

        // non-standard: used by NeoVIM
        String{ {}, "setrgbf"sv, "\E[38:2:%p1%d:%p2%d:%p3%dm"sv }, // setrgbf: Set RGB foreground color
        String{ {}, "setrgbb"sv, "\E[48:2:%p1%d:%p2%d:%p3%dm"sv }, // setrgbb: Set RGB background color

        // Inputs (TODO: WIP!)
        String{ "ku"_tcap, "kcuu1"sv, "\EOA"sv }, // app: cursor up
        String{ "kr"_tcap, "kcuf1"sv, "\EOC"sv }, // app: cursor right
        String{ "kl"_tcap, "kcub1"sv, "\EOD"sv }, // app: cursor left
        String{ "kd"_tcap, "kcud1"sv, "\EOB"sv }, // app: cursor left
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
