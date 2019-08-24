# libterminal - A modern C++ terminal emulator library
[![CircleCI](https://circleci.com/gh/christianparpart/libterminal.svg?style=svg)](https://circleci.com/gh/christianparpart/libterminal)
[![codecov](https://codecov.io/gh/christianparpart/libterminal/branch/master/graph/badge.svg)](https://codecov.io/gh/christianparpart/libterminal)
[![C++17](https://img.shields.io/badge/standard-C%2B%2B%2017-blue.svg?logo=C%2B%2B)](https://isocpp.org/)

**IMPORANT: THIS PROJECT IS IN ALPHA STAGE & ACTIVE DEVELOPMENT**

## MIlestone-1:
- [x] Process: PTY-aware Process API
- [x] Parser: parsing of VT100-VT520
- [x] OutputHandler: basic VT output handling
- [x] demo PTY client app that's just proxying all process data and performing a full screen redraw upon updates
- [x] Process: Windows Platform support
- [ ] Screen management done (as much as needed to run: top, htop, mc, vim, tmux, bash)
- [ ] InputHandler: basic VT input handling (as much as needed to run: top, htop, mc, vim, tmux, bash)
- [ ] Terminal: fully functioning Terminal API
- [ ] => initial release 0.1.0

## MIlestone-2:
- [ ] Mouse support
- [ ] OutputHandler: support most VT100-VT520 control functions
- [ ] Screen: support most VT100-VT520 control functions
- [ ] Tests: almost complete unit tests
- [ ] Unicode: multi codepoint grapheme support
- [ ] Telemetry: control function usage counts, error counts, warning counts
- [ ] => release 0.2.0

### Users

* [contour](https://github.com/christianparpart/contour).

# References

- [VT510](https://vt100.net/docs/vt510-rm/): VT510 Manual, see Chapter 5.
- [ECMA-35](http://www.ecma-international.org/publications/standards/Ecma-035.htm):
    Character Code Structure and Extension Techniques
- [ECMA-43](http://www.ecma-international.org/publications/standards/Ecma-043.htm):
    8-bit Coded Character Set Structure and Rules
- [ECMA-48](http://www.ecma-international.org/publications/standards/Ecma-048.htm):
    Control Functions for Coded Character Sets
- [ISO/IEC 8613-6](https://www.iso.org/standard/22943.html):
    Character content architectures
- [xterm](https://invisible-island.net/xterm/ctlseqs/ctlseqs.html): xterm control sequences
- [console\_codes](http://man.he.net/man4/console_codes) Linux console codes
- [Summary of ANSI standards for ASCII terminals](http://www.inwap.com/pdp10/ansicode.txt)
