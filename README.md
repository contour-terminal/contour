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
- [ ] Screen management done
- [ ] InputHandler: basic VT input handling
- [ ] Terminal: fully functioning Terminal API
- [ ] App Support Checklist: top, htop, mc, vim, tmux
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
