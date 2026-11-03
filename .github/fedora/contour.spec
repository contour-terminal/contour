# This enables cmake to work consistently between pre-f33 and after.
# https://docs.fedoraproject.org/en-US/packaging-guidelines/CMake/#_notes
%undefine __cmake_in_source_build

# Get contour version
%{!?_version: %define _version %{getenv:CONTOUR_VERSION} }

# One spec, two packages. The default build is the GUI terminal, `contour`. `rpmbuild --with daemon` builds
# `contour-daemon` instead: the GUI-less configuration (-DCONTOUR_FRONTEND_GUI=OFF) -- a session host for a
# server or a container -- that links no Qt and pulls none in. The two ship the same /usr/bin/contour and so
# conflict with each other, exactly as the Debian packages do.
#
# Target distributions: current Fedora and rawhide. EL9 is out of reach (its GCC 11 cannot build the C++23
# code; it would need gcc-toolset on top of EPEL's yaml-cpp and libutempter), and EL10 is untried: no job
# builds either, so none is claimed.
%bcond_with daemon

# The source tarball, its top-level directory and the project URL are named after the project, not after
# the package: the daemon package is called contour-daemon, but the sources it is built from are contour's.
%global upstream_name contour

%if %{with daemon}
Name:           contour-daemon
%else
Name:           contour
%endif
Version:        %{_version}
Release:        1%{?dist}
%if %{with daemon}
Summary:        Contour terminal session daemon (no GUI)
%else
Summary:        Modern C++ Terminal Emulator
%endif

License:        ASL 2.0
URL:            https://github.com/contour-terminal/%{upstream_name}
Source0:        %{upstream_name}-%{version}.tar.gz

BuildRequires:  cmake
BuildRequires:  extra-cmake-modules
BuildRequires:  fontconfig-devel
BuildRequires:  freetype-devel
BuildRequires:  gcc-c++
BuildRequires:  harfbuzz-devel
BuildRequires:  libssh2-devel
BuildRequires:  libutempter-devel
BuildRequires:  ninja-build
BuildRequires:  pkgconf
BuildRequires:  systemd-devel
BuildRequires:  yaml-cpp-devel
%if %{without daemon}
# The keyboard pipeline's libxkbcommon adapter exists in the GUI build only (src/contour/input).
BuildRequires:  libxkbcommon-devel
BuildRequires:  libxkbcommon-x11-devel
BuildRequires:  qt6-qtbase-devel
BuildRequires:  qt6-qtbase-private-devel
BuildRequires:  qt6-qtbase-gui
BuildRequires:  qt6-qtdeclarative-devel
BuildRequires:  qt6-qtmultimedia-devel
BuildRequires:  qt6-qtshadertools-devel
%endif

Requires:       cairo
Requires:       fontconfig
Requires:       freetype
Requires:       harfbuzz
Requires:       libssh2
Requires:       libutempter
Requires:       systemd-libs
Requires:       yaml-cpp
%if %{with daemon}
# The same binary and the same files as the GUI package (and the name some distributions give it).
Conflicts:      contour
Conflicts:      contour-terminal
%else
Requires:       libxkbcommon
Requires:       libxkbcommon-x11
Requires:       qt6-qtbase
Requires:       qt6-qtbase-gui
Requires:       qt6-qtmultimedia
Requires:       qt6-qtsvg
Conflicts:      contour-daemon
%endif

%description
%if %{with daemon}
Contour is a modern and actually fast, modal, virtual terminal emulator. This package is its
GUI-less build: `contour daemon` hosts persistent terminal sessions for a server or a container,
and links no Qt.
%else
Contour is a modern and actually fast, modal, virtual terminal emulator,
for everyday use. It is aiming for power users with a modern feature mindset.
%endif


%prep
%setup -q -n %{upstream_name}-%{version}


%build
# The bundled status-line extensions are not built: cargo fetches their crates (wit-bindgen) from
# crates.io, and mock/koji build offline. Vendoring the crates into the SRPM is a packaging follow-up;
# until then the package ships the extension runtime without the bundled modules.
#
# The daemon build mirrors the Debian daemon package: no terminfo entry (CONTOUR_PACKAGE_TERMINFO=OFF;
# the reason there is inferred, not verified for Fedora -- the ncurses packages may already ship it and a
# second copy would conflict on install; a host without the entry falls back to TERM=xterm-256color per
# Config.cpp, so the downside is mild), and one link at a time (CONTOUR_LINK_JOBS=1), after linking
# shut down the Debian job's runner for lack of memory. The GUI package is built as it always was.
cmake . \
%if %{with daemon}
    -DCONTOUR_FRONTEND_GUI=OFF \
    -DCMAKE_DISABLE_FIND_PACKAGE_Qt6=ON \
    -DCONTOUR_PACKAGE_TERMINFO=OFF \
    -DCONTOUR_LINK_JOBS=1 \
%endif
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DPEDANTIC_COMPILER=ON \
    -DPEDANTIC_COMPILER_WERROR=ON \
    -DCONTOUR_TESTING=ON \
    -DCONTOUR_EXTENSIONS_BUNDLED=OFF \
    -B build \
    -GNinja
cd build
%if %{with daemon}
# The targets the Debian daemon job builds, plus the two suites %check runs, named so they build first
# (and under CONTOUR_LINK_JOBS=1). %ninja_install below still builds whatever else the install needs.
%ninja_build contour contour_test crispy_test vtbackend_test
%else
%ninja_build
%endif


%install
cd build
%ninja_install

# TODO: Make this an if-statement to be decided via env var from the outside.
# verify desktop file (not possible in github actions)
# desktop-file-validate %{buildroot}%{_datadir}/applications/%{name}.desktop


%check
./build/src/crispy/crispy_test
./build/src/vtbackend/vtbackend_test


%files
%license LICENSE.txt
%doc README.md docs/CONTRIBUTING.md
%{_bindir}/*
%{_datadir}/*
# The daemon as a systemd user service: shipped in both packages, enabled by neither.
%{_prefix}/lib/systemd/user/contour-daemon.service
%if %{without daemon}
# The systemd user drop-ins that keep Contour's own unit running through an out-of-memory kill inside it:
# one directory per unit name the desktops' launchers use (systemd reads lib, never lib64). Desktop-only,
# so the daemon package has none.
%{_prefix}/lib/systemd/user/app-*org.contourterminal.Contour*.d/
%endif


%changelog
