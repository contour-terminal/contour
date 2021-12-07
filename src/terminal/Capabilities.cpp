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
#include <crispy/escape.h>
#include <sstream>
#include <range/v3/action/sort.hpp>
#include <range/v3/action/transform.hpp>
#include <range/v3/algorithm/copy.hpp>
#include <range/v3/view/transform.hpp>
#include <range/v3/view/concat.hpp>

using std::nullopt;
using std::optional;
using std::string;
using std::string_view;
using std::u32string_view;

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

template <typename T> constexpr bool operator<(Cap<T> const& a, Cap<T> const& b) noexcept { return a.name < b.name; }
template <typename T> constexpr bool operator>(Cap<T> const& a, Cap<T> const& b) noexcept { return a.name > b.name; }
template <typename T> constexpr bool operator<=(Cap<T> const& a, Cap<T> const& b) noexcept { return a.name <= b.name; }
template <typename T> constexpr bool operator>=(Cap<T> const& a, Cap<T> const& b) noexcept { return a.name >= b.name; }
template <typename T> constexpr bool operator==(Cap<T> const& a, Cap<T> const& b) noexcept { return a.name == b.name; }
template <typename T> constexpr bool operator!=(Cap<T> const& a, Cap<T> const& b) noexcept { return a.name == b.name; }

using Boolean = Cap<bool>;
using Numeric = Cap<int>;
using String = Cap<std::string_view>;

namespace
{
    template <typename T, typename... Ts>
    constexpr auto defineCapabilities(T _element, Ts... _elements)
    {
        return std::array<T, 1 + sizeof...(Ts)>({_element, _elements...});
    }

    constexpr inline auto booleanCaps = defineCapabilities(
        Boolean{ "Su"_tcap, "Su"sv, true },  // supports extended underline styling (such as undercurl)
        Boolean{ "am"_tcap, "am"sv, true },  // terminal has automatic margins
        Boolean{ "ut"_tcap, "bce"sv, true },  // screen erased with background color
        Boolean{ "cc"_tcap, "ccc"sv, true },  // terminal can re-define existing colors
        Boolean{ "xn"_tcap, "xenl"sv, true },  // newline ignored after 80 cols (concept)
        Boolean{ "km"_tcap, "km"sv, true },  // Has a meta key (i.e., sets 8th-bit)
        Boolean{ "mi"_tcap, "mir"sv, true },  // safe to move while in insert mode
        Boolean{ "ms"_tcap, "msgr"sv, true },  // safe to move while in standout mode
        Boolean{ "NP"_tcap, "npc"sv, true },  // pad character does not exist
        Boolean{ "5i"_tcap, "mc5i"sv, true },  // printer will not echo on screen
        Boolean{ "YD"_tcap, "xvpa"sv, true },  // only positive motion for vpa/mvpa caps
        Boolean{ "Tc"_tcap, "Tc"sv, true }  // RGB color support (introduced by Tmux in 2016)
    );

    constexpr inline auto numericalCaps = defineCapabilities(
        Numeric{ "co"_tcap, "cols"sv, 80 },   // number of columns in a line
        Numeric{ "it"_tcap, "it"sv, 8 },    // tabs initially every # spaces
        Numeric{ "Co"_tcap, "colors"sv, 256 },  // maximum number of colors on screen
        Numeric{ "pa"_tcap, "pairs"sv, 32767 }  // maximum number of color-pairs on the screen
    );

    constexpr auto inline Undefined = Code{};
    constexpr inline auto stringCaps = defineCapabilities( // {{{
        String{ "TN"_tcap, ""sv, "xterm-256color"sv }, // termcap/terminfo name (xterm extension)
        String{ "ac"_tcap, "acsc"sv, "``aaffggiijjkkllmmnnooppqqrrssttuuvvwwxxyyzz{{||}}~~"sv }, // graphics charset pairs, based on vt100
        String{ "bl"_tcap, "bel"sv, "^G"sv }, // The audible bell character
        String{ "md"_tcap, "bold"sv, "\033[1m"sv }, // Escape code for bold
        String{ "bt"_tcap, "cbt"sv, "\033[Z"sv }, // Back tab
        String{ "kB"_tcap, "kcbt"sv, "\033[Z"sv },
        String{ "vi"_tcap, "civis"sv, "\033[?25l"sv }, // Make cursor invisible
        String{ "cl"_tcap, "clear"sv, "\033[H\033[2J"sv }, // Clear screen
        String{ "ve"_tcap, "cnorm"sv, "\033[?12l\033[?25h"sv }, // Make cursor appear normal
        String{ "cr"_tcap, "cr"sv, "^M"sv },  // CR (carriage return \r)
        String{ "cs"_tcap, "csr"sv, "\033[%i%p1%d;%p2%dr"sv }, // Change scroll region
        String{ "LE"_tcap, "cub"sv, "\033[%p1%dD"sv }, // // Move cursor to the left by the specified amount
        String{ "le"_tcap, "cub1"sv, "^H"sv },  // BS (backspace)
        // Move cursor down specified number of lines
        String{ "DO"_tcap, "cud"sv, "\033[%p1%dB"sv },
        String{ "do"_tcap, "cud1"sv, "^J"sv },  // LF (line-feed \n)
        // Move cursor to the right by the specified amount
        String{ "RI"_tcap, "cuf"sv, "\033[%p1%dC"sv },
        String{ "nd"_tcap, "cuf1"sv, "\033[C"sv },
        // Move cursor up specified number of lines
        String{ "UP"_tcap, "cuu"sv,"" "\033[%p1%dA"sv },
        String{ "up"_tcap, "cuu1"sv,"" "\033[A"sv },
        // Move cursor to specified location
        String{ "cm"_tcap, "cup"sv, "\033[%i%p1%d;%p2%dH"sv },
        // Make cursor very visible
        String{ "vs"_tcap, "cvvis"sv, "\033[?12;25h"sv },
        // Delete the specified number of characters
        String{ "DC"_tcap, "dch"sv, "\033[%p1%dP"sv },
        String{ "dc"_tcap, "dch1"sv, "\033[P"sv },
        // Turn on half bright mode
        String{ "mh"_tcap, "dim"sv,"" "\033[2m"sv },
        // Delete the specified number of lines
        String{ "DL"_tcap, "dl"sv, "\033[%p1%dM"sv },
        String{ "dl"_tcap, "dl1"sv,"" "\033[M"sv },
        // Erase specified number of characters
        String{ "ec"_tcap, "ech"sv, "\033[%p1%dX"sv },
        // Clear to end of screen
        String{ "cd"_tcap, "ed"sv, "\033[J"sv },
        String{ "ce"_tcap, "el"sv, "\033[K"sv }, // Clear to end of line
        String{ "cb"_tcap, "el1"sv, "\033[1K"sv }, // Clear to start of line
        String{ "vb"_tcap, "flash"sv, "\033[?5h$<100/>\033[?5l"sv },   // visible bell
        String{ "ho"_tcap, "home"sv, "\033[H"sv },                  // Home cursor
        String{ "ch"_tcap, "hpa"sv, "\033[%i%p1%dG"sv },           // Move cursor to column
        String{ "ht"_tcap, "ht"sv, "^I"sv },                    // Move to next tab
        String{ "st"_tcap, "hts"sv, "\033H"sv },                   // Set tabstop at current position
        String{ "IC"_tcap, "ich"sv, "\033[%p1%d@"sv },             // Insert specified number of characters
        String{ "AL"_tcap, "il"sv, "\033[%p1%dL"sv },             // insert #1 lines (P*)
        String{ "al"_tcap, "il1"sv, "\033[L"sv },                  // insert line (P*)
        String{ "sf"_tcap, "ind"sv, "^J"sv },                    // scroll up by specified amount
        String{ "SF"_tcap, "indn"sv,"" "\033[%p1%dS"sv },             // scroll forward #1 lines (P)
        // initialize color (set dynamic colors)
        String{ "Ic"_tcap, "initc"sv, "\033]4;%p1%d;rgb:%p2%{255}%*%{1000}%/%2.2X/%p3%{255}%*%{1000}%/%2.2X/%p4%{255}%*%{1000}%/%2.2X\033\\"sv },
        // Set all colors to original values
        String{ "oc"_tcap, "oc"sv, "\033]104\033\\"sv },
        // turn on blank mode (characters invisible)
        String{ "mk"_tcap, "invis"sv, "\033[8m"sv },     // turn on blank mode (characters invisible)
        String{ "kb"_tcap, "kbs"sv, "\177"sv }, // Backspace
        String{ "Km"_tcap, "kmous"sv, "\033[M"sv }, // Mouse event has occurred
        String{ "kR"_tcap, "kri"sv, "\033[1;2A"sv }, // Scroll backwards (reverse index)
        String{ "kF"_tcap, "kind"sv, "\033[1;2B"sv }, // scroll forwards (index)
        String{ "rc"_tcap, "rc"sv, "\0338"sv }, // Restore cursor
        String{ "rp"_tcap, "rep"sv, "%p1%c\033[%p2%{1}%-%db"sv }, // Repeat preceding character
        String{ "mr"_tcap, "rev"sv,"" "\033[7m"sv }, // Reverse video
        String{ "sr"_tcap, "ri"sv, "\033M"sv }, // Scroll backwards the specified number of lines (reverse index)
        String{ "SR"_tcap, "rin"sv, "\033[%p1%dT"sv },
        String{ "RA"_tcap, "rmam"sv, "\033[?7l"sv }, // Turn off automatic margins
        String{ "te"_tcap, "rmcup"sv,"" "\033[?1049l"sv }, // Exit alternate screen
        String{ "ei"_tcap, "rmir"sv, "\033[4l"sv }, // Exit insert mode
        String{ "ke"_tcap, "rmkx"sv, "\033[?1l"sv }, // Exit application keypad mode
        String{ "se"_tcap, "rmso"sv, "\033[27m"sv }, // Exit standout mode
        String{ "ue"_tcap, "rmul"sv, "\033[24m"sv }, // Exit underline mode
        String{ "Te"_tcap, "rmxx"sv, "\033[29m"sv }, // Exit strikethrough mode
        String{ "r1"_tcap, "rs1"sv,"" "\033]\033\\\033c"sv }, // Reset string1 (empty OSC sequence to exit OSC/OTH modes, and regular reset)
        String{ "sc"_tcap, "sc"sv, "\0337"sv }, // Save cursor
        String{ "AB"_tcap, "setab"sv, "\033[%?%p1%{8}%<%t4%p1%d%e%p1%{16}%<%t10%p1%{8}%-%d%e48;5;%p1%d%;m"sv }, // Set background color
        String{ "AF"_tcap, "setaf"sv, "\033[%?%p1%{8}%<%t3%p1%d%e%p1%{16}%<%t9%p1%{8}%-%d%e38;5;%p1%d%;m"sv }, // Set foreground color
        String{ "sa"_tcap, "sgr"sv, "%?%p9%t\033(0%e\033(B%;\033[0%?%p6%t;1%;%?%p2%t;4%;%?%p1%p3%|%t;7%;%?%p4%t;5%;%?%p7%t;8%;m"sv }, // Set attributes
        String{ "me"_tcap, "sgr0"sv, "\033(B\033[m"sv },     // Clear all attributes
        String{ "op"_tcap, "op"sv, "\033[39;49m"sv },   // Reset color pair to its original value
        String{ "SA"_tcap, "smam"sv,"" "\033[?7h"sv },      // Turn on automatic margins
        String{ "ti"_tcap, "smcup"sv, "\033[?1049h"sv },   // Start alternate screen
        String{ "im"_tcap, "smir"sv,"" "\033[4h"sv },       // Enter insert mode
        String{ "ks"_tcap, "smkx"sv, "\033[?1h"sv },      // Enter application keymap mode
        String{ "so"_tcap, "smso"sv, "\033[7m"sv },       // Enter standout mode
        String{ "us"_tcap, "smul"sv, "\033[4m"sv },       // Enter underline mode
        String{ "Ts"_tcap, "smxx"sv, "\033[9m"sv },       // Enter strikethrough mode
        String{ "ct"_tcap, "tbc"sv, "\033[3g"sv },       // Clear all tab stops
        String{ "ts"_tcap, "tsl"sv, "\033]2;"sv },       // To status line (used to set window titles)
        String{ "fs"_tcap, "fsl"sv, "^G"sv },          // From status line (end window title string)
        String{ "ds"_tcap, "dsl"sv,"" "\033]2;\033\\"sv },   // Disable status line (clear window title)
        String{ "cv"_tcap, "vpa"sv,"" "\033[%i%p1%dd"sv }, // Move to specified line
        String{ "ZH"_tcap, "sitm"sv, "\033[3m"sv },       // Enter italics mode
        String{ "ZR"_tcap, "ritm"sv,"" "\033[23m"sv },      // Leave italics mode
        String{ "as"_tcap, "smacs"sv,"" "\033(0"sv }, // start alternate character set (P)
        String{ "ae"_tcap, "rmacs"sv,"" "\033(B"sv }, // end alternate character set (P)

        // Synchronized Output
        String{ Undefined, "Sync"sv, "\033[?2026%?%p1%{1}%-%tl%eh"sv },

        // non-standard: used by NeoVIM
        String{ Undefined, "setrgbf"sv, "\033[38:2:%p1%d:%p2%d:%p3%dm"sv }, // setrgbf: Set RGB foreground color
        String{ Undefined, "setrgbb"sv, "\033[48:2:%p1%d:%p2%d:%p3%dm"sv }, // setrgbb: Set RGB background color

        // Inputs (TODO: WIP!)
	    String{ "*4"_tcap, "kDC"sv, "\033[3;2~"sv },
	    String{ Undefined, "kDC3"sv, "\033[3;3~"sv },
	    String{ Undefined, "kDC4"sv, "\033[3;4~"sv },
	    String{ Undefined, "kDC5"sv, "\033[3;5~"sv },
	    String{ Undefined, "kDC6"sv, "\033[3;6~"sv },
	    String{ Undefined, "kDC7"sv, "\033[3;7~"sv },
	    String{ Undefined, "kDN"sv, "\033[1;2B"sv },
	    String{ Undefined, "kDN3"sv, "\033[1;3B"sv },
	    String{ Undefined, "kDN4"sv, "\033[1;4B"sv },
	    String{ Undefined, "kDN5"sv, "\033[1;5B"sv },
	    String{ Undefined, "kDN6"sv, "\033[1;6B"sv },
	    String{ Undefined, "kDN7"sv, "\033[1;7B"sv },
	    String{ "*7"_tcap, "kEND"sv, "\033[1;2F"sv },
	    String{ Undefined, "kEND3"sv, "\033[1;3F"sv },
	    String{ Undefined, "kEND4"sv, "\033[1;4F"sv },
	    String{ Undefined, "kEND5"sv, "\033[1;5F"sv },
	    String{ Undefined, "kEND6"sv, "\033[1;6F"sv },
	    String{ Undefined, "kEND7"sv, "\033[1;7F"sv },
	    String{ "#2"_tcap, "kHOM"sv, "\033[1;2H"sv },
	    String{ Undefined, "kHOM3"sv, "\033[1;3H"sv },
	    String{ Undefined, "kHOM4"sv, "\033[1;4H"sv },
	    String{ Undefined, "kHOM5"sv, "\033[1;5H"sv },
	    String{ Undefined, "kHOM6"sv, "\033[1;6H"sv },
	    String{ Undefined, "kHOM7"sv, "\033[1;7H"sv },
	    String{ "#3"_tcap, "kIC"sv, "\033[2;2~"sv },
	    String{ Undefined, "kIC3"sv, "\033[2;3~"sv },
	    String{ Undefined, "kIC4"sv, "\033[2;4~"sv },
	    String{ Undefined, "kIC5"sv, "\033[2;5~"sv },
	    String{ Undefined, "kIC6"sv, "\033[2;6~"sv },
	    String{ Undefined, "kIC7"sv, "\033[2;7~"sv },
	    String{ "#4"_tcap, "kLFT"sv, "\033[1;2D"sv },
	    String{ Undefined, "kLFT3"sv, "\033[1;3D"sv },
	    String{ Undefined, "kLFT4"sv, "\033[1;4D"sv },
	    String{ Undefined, "kLFT5"sv, "\033[1;5D"sv },
	    String{ Undefined, "kLFT6"sv, "\033[1;6D"sv },
	    String{ Undefined, "kLFT7"sv, "\033[1;7D"sv },
	    String{ "%c"_tcap, "kNXT"sv, "\033[6;2~"sv },
	    String{ Undefined, "kNXT3"sv, "\033[6;3~"sv },
	    String{ Undefined, "kNXT4"sv, "\033[6;4~"sv },
	    String{ Undefined, "kNXT5"sv, "\033[6;5~"sv },
	    String{ Undefined, "kNXT6"sv, "\033[6;6~"sv },
	    String{ Undefined, "kNXT7"sv, "\033[6;7~"sv },
	    String{ "%e"_tcap, "kPRV"sv, "\033[5;2~"sv },
	    String{ Undefined, "kPRV3"sv, "\033[5;3~"sv },
	    String{ Undefined, "kPRV4"sv, "\033[5;4~"sv },
	    String{ Undefined, "kPRV5"sv, "\033[5;5~"sv },
	    String{ Undefined, "kPRV6"sv, "\033[5;6~"sv },
	    String{ Undefined, "kPRV7"sv, "\033[5;7~"sv },
	    String{ "%i"_tcap, "kRIT"sv, "\033[1;2C"sv },
	    String{ Undefined, "kRIT3"sv, "\033[1;3C"sv },
	    String{ Undefined, "kRIT4"sv, "\033[1;4C"sv },
	    String{ Undefined, "kRIT5"sv, "\033[1;5C"sv },
	    String{ Undefined, "kRIT6"sv, "\033[1;6C"sv },
	    String{ Undefined, "kRIT7"sv, "\033[1;7C"sv },
	    String{ Undefined, "kUP"sv, "\033[1;2A"sv },
	    String{ Undefined, "kUP3"sv, "\033[1;3A"sv },
	    String{ Undefined, "kUP4"sv, "\033[1;4A"sv },
	    String{ Undefined, "kUP5"sv, "\033[1;5A"sv },
	    String{ Undefined, "kUP6"sv, "\033[1;6A"sv },
	    String{ Undefined, "kUP7"sv, "\033[1;7A"sv },
	    String{ "K1"_tcap, "ka1"sv, ""sv }, // upper left of keypad
	    String{ "K3"_tcap, "ka3"sv, ""sv }, // upper right of keypad
	    String{ "K4"_tcap, "kc1"sv, ""sv }, // center of keypad
	    String{ "K5"_tcap, "kc3"sv, ""sv }, // lower right of keypad
        String{ "kl"_tcap, "kcub1"sv, "\033OD"sv }, // app: cursor left
        String{ "kd"_tcap, "kcud1"sv, "\033OB"sv }, // app: cursor left
        String{ "kr"_tcap, "kcuf1"sv, "\033OC"sv }, // app: cursor right
        String{ "ku"_tcap, "kcuu1"sv, "\033OA"sv }, // app: cursor up
	    String{ "kD"_tcap, "kdch1"sv, "\033[3~"sv },
	    String{ "@7"_tcap, "kend"sv, "\033OF"sv },
	    String{ "k1"_tcap, "kf1"sv, "\033OP"sv },
	    String{ "k;"_tcap, "kf10"sv, "\033[21~"sv },
	    String{ "F1"_tcap, "kf11"sv, "\033[23~"sv },
	    String{ "F2"_tcap, "kf12"sv, "\033[24~"sv },
	    String{ "F3"_tcap, "kf13"sv, "\033[1;2P"sv },
	    String{ "F4"_tcap, "kf14"sv, "\033[1;2Q"sv },
	    String{ "F5"_tcap, "kf15"sv, "\033[1;2R"sv },
	    String{ "F6"_tcap, "kf16"sv, "\033[1;2S"sv },
	    String{ "F7"_tcap, "kf17"sv, "\033[15;2~"sv },
	    String{ "F8"_tcap, "kf18"sv, "\033[17;2~"sv },
	    String{ "F9"_tcap, "kf19"sv, "\033[18;2~"sv },
	    String{ "k2"_tcap, "kf2"sv, "\033OQ"sv },
	    String{ "FA"_tcap, "kf20"sv, "\033[19;2~"sv },
	    String{ "FB"_tcap, "kf21"sv, "\033[20;2~"sv },
	    String{ "FC"_tcap, "kf22"sv, "\033[21;2~"sv },
	    String{ "FD"_tcap, "kf23"sv, "\033[23;2~"sv },
	    String{ "FE"_tcap, "kf24"sv, "\033[24;2~"sv },
	    String{ "FF"_tcap, "kf25"sv, "\033[1;5P"sv },
	    String{ "FG"_tcap, "kf26"sv, "\033[1;5Q"sv },
	    String{ "FH"_tcap, "kf27"sv, "\033[1;5R"sv },
	    String{ "FI"_tcap, "kf28"sv, "\033[1;5S"sv },
	    String{ "FJ"_tcap, "kf29"sv, "\033[15;5~"sv },
	    String{ "k3"_tcap, "kf3"sv, "\033OR"sv },
	    String{ "FK"_tcap, "kf30"sv, "\033[17;5~"sv },
	    String{ "FL"_tcap, "kf31"sv, "\033[18;5~"sv },
	    String{ "FM"_tcap, "kf32"sv, "\033[19;5~"sv },
	    String{ "FN"_tcap, "kf33"sv, "\033[20;5~"sv },
	    String{ "FO"_tcap, "kf34"sv, "\033[21;5~"sv },
	    String{ "FP"_tcap, "kf35"sv, "\033[23;5~"sv },
	    String{ "FQ"_tcap, "kf36"sv, "\033[24;5~"sv },
	    String{ "FR"_tcap, "kf37"sv, "\033[1;6P"sv },
	    String{ "FS"_tcap, "kf38"sv, "\033[1;6Q"sv },
	    String{ "FT"_tcap, "kf39"sv, "\033[1;6R"sv },
	    String{ "k4"_tcap, "kf4"sv, "\033OS"sv },
	    String{ "FU"_tcap, "kf40"sv, "\033[1;6S"sv },
	    String{ "FV"_tcap, "kf41"sv, "\033[15;6~"sv },
	    String{ "FW"_tcap, "kf42"sv, "\033[17;6~"sv },
	    String{ "FX"_tcap, "kf43"sv, "\033[18;6~"sv },
	    String{ "FY"_tcap, "kf44"sv, "\033[19;6~"sv },
	    String{ "FZ"_tcap, "kf45"sv, "\033[20;6~"sv },
	    String{ "Fa"_tcap, "kf46"sv, "\033[21;6~"sv },
	    String{ "Fb"_tcap, "kf47"sv, "\033[23;6~"sv },
	    String{ "Fc"_tcap, "kf48"sv, "\033[24;6~"sv },
	    String{ "Fd"_tcap, "kf49"sv, "\033[1;3P"sv },
	    String{ "k5"_tcap, "kf5"sv, "\033[15~"sv },
	    String{ "Fe"_tcap, "kf50"sv, "\033[1;3Q"sv },
	    String{ "Ff"_tcap, "kf51"sv, "\033[1;3R"sv },
	    String{ "Fg"_tcap, "kf52"sv, "\033[1;3S"sv },
	    String{ "Fh"_tcap, "kf53"sv, "\033[15;3~"sv },
	    String{ "Fi"_tcap, "kf54"sv, "\033[17;3~"sv },
	    String{ "Fj"_tcap, "kf55"sv, "\033[18;3~"sv },
	    String{ "Fk"_tcap, "kf56"sv, "\033[19;3~"sv },
	    String{ "Fl"_tcap, "kf57"sv, "\033[20;3~"sv },
	    String{ "Fm"_tcap, "kf58"sv, "\033[21;3~"sv },
	    String{ "Fn"_tcap, "kf59"sv, "\033[23;3~"sv },
	    String{ "k6"_tcap, "kf6"sv, "\033[17~"sv },
	    String{ "Fo"_tcap, "kf60"sv, "\033[24;3~"sv },
	    String{ "Fp"_tcap, "kf61"sv, "\033[1;4P"sv },
	    String{ "Fq"_tcap, "kf62"sv, "\033[1;4Q"sv },
	    String{ "Fr"_tcap, "kf63"sv, "\033[1;4R"sv },
	    String{ "k7"_tcap, "kf7"sv, "\033[18~"sv },
	    String{ "k8"_tcap, "kf8"sv, "\033[19~"sv },
	    String{ "k9"_tcap, "kf9"sv, "\033[20~"sv },
	    String{ "%1"_tcap, "khlp"sv, ""sv },
	    String{ "kh"_tcap, "khome"sv, "\033OH"sv },
	    String{ "kI"_tcap, "kich1"sv, "\033[2~"sv },
	    String{ "Km"_tcap, "kmous"sv, "\033[M"sv },
	    String{ "kN"_tcap, "knp"sv, "\033[6~"sv },
	    String{ "kP"_tcap, "kpp"sv, "\033[5~"sv },
	    String{ "&8"_tcap, "kund"sv, ""sv },

        // {{{ Extensions originally introduced by tmux.
        //
        String{ "Ss"_tcap, "Ss"sv, "\033[%p1%d q" },    // Set cursor style.
        String{ "Se"_tcap, "Ss"sv, "\033[ q" },         // Reset cursor style.

        // Set cursor color.
        String{ "Cs"_tcap, "Cs"sv, "\033]12;%p1%s\033\\"sv },

        // Reset cursor color.
        // TODO String{ "Cr"_tcap, "Cr"sv, ""sv },

        // Enable overline SGR attribute.
        String{ Undefined, "Smol"sv, "\033[53m"sv  },

        // Set styled underscore.
        String{ Undefined, "Smulx"sv, "\033[4:%p1%dm"sv },

        // Set underscore color.
        String{ Undefined, "Setulc"sv, "\033[58:2:%p1%{65536}%/%d:%p1%{256}%/%{255}%&%d:%p1%{255}%&%d%;m"sv },
        // NB: the following would work, too.
        // String{ Undefined, "Setulc"sv, "\033[58:2:%p1%d:%p2%d:%p3%dm"sv },
        // }}}

        // RGB for the ncurses direct-color extension.
        // Only a terminfo name is provided, since termcap applica-
        // tions cannot use this information
        String{ Undefined, "RGB"sv, "8/8/8"sv }
    ); // }}}
}

bool StaticDatabase::booleanCapability(Code _cap) const
{
    for (auto const& cap: booleanCaps)
        if (cap.code.code == _cap.code)
            return cap.value;

    return false;
}

unsigned StaticDatabase::numericCapability(Code _cap) const
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

bool StaticDatabase::booleanCapability(string_view _cap) const
{
    for (auto const tcap: booleanCaps)
        if (tcap.name == _cap || tcap.code == _cap)
            return tcap.value;

    return false;
}

unsigned StaticDatabase::numericCapability(string_view _cap) const
{
    for (auto const tcap: numericalCaps)
        if (tcap.name == _cap || tcap.code == _cap)
            return tcap.value;

    return -1;
}

string_view StaticDatabase::stringCapability(string_view _cap) const
{
    for (auto const tcap: stringCaps)
        if (tcap.name == _cap || tcap.code == _cap)
            return tcap.value;

    return {};
}

optional<Code> StaticDatabase::codeFromName(string_view _name) const
{
    using ranges::views::concat;
    using ranges::views::transform;
    auto const tr = transform([](auto cap) { return std::pair{cap.name, cap.code}; });
    for (auto const cap: concat(numericalCaps | tr, booleanCaps | tr, stringCaps | tr))
        if (cap.first == _name)
            return cap.second;

    return nullopt;
}

string StaticDatabase::terminfo() const
{
    using namespace ranges;

    auto booleans = copy(booleanCaps);
    auto numbers = copy(numericalCaps);
    auto strings = copy(stringCaps);

    std::stringstream output;

    output << "contour|contour-latest|Contour Terminal Emulator,\n";

    for (auto const& cap: move(booleans) | actions::sort)
        if (!cap.name.empty() && cap.value)
            output << "    " << cap.name << ",\n";

    for (auto const& cap: move(numbers) | actions::sort)
        if (!cap.name.empty())
            output << "    " << cap.name << "#" << cap.value << ",\n";

    for (auto const& cap: move(strings) | actions::sort)
        if (!cap.name.empty())
            output << "    " << cap.name << "=" << crispy::escape(cap.value) << ",\n";

    return output.str();
}

}
