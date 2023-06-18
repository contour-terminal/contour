# This enables cmake to work consistently between pre-f33 and after.
# https://docs.fedoraproject.org/en-US/packaging-guidelines/CMake/#_notes
%undefine __cmake_in_source_build

# Get contour version
%{!?_version: %define _version %{getenv:CONTOUR_VERSION} }

Name:           contour
Version:        %{_version}
Release:        1%{?dist}
Summary:        Modern C++ Terminal Emulator

License:        ASL 2.0
URL:            https://github.com/contour-terminal/%{name}
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  cmake
BuildRequires:  extra-cmake-modules
# Don't use system-dep of fmt-devel for now as we need a newer one
# BuildRequires:  fmt-devel
BuildRequires:  fontconfig-devel
BuildRequires:  freetype-devel
BuildRequires:  gcc-c++
BuildRequires:  harfbuzz-devel
BuildRequires:  ninja-build
BuildRequires:  pkgconf
BuildRequires:  qt6-qtbase-devel
BuildRequires:  qt6-qtbase-gui
BuildRequires:  qt6-qtdeclarative-devel
BuildRequires:  qt6-qtmultimedia-devel

Requires:       fontconfig
Requires:       freetype
Requires:       harfbuzz
Requires:       qt6-qtbase
Requires:       qt6-qtbase-gui
Requires:       qt6-qtmultimedia-devel
Requires:       yaml-cpp

%description
Contour is a modern and actually fast, modal, virtual terminal emulator,
for everyday use. It is aiming for power users with a modern feature mindset.


%prep
%setup -q -n %{name}-%{version}


%build
cmake . \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCONTOUR_QT_VERSION=6 \
    -DPEDANTIC_COMPILER=ON \
    -DPEDANTIC_COMPILER_WERROR=ON \
    -B build \
    -GNinja
cd build
%ninja_build


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
%doc README.md docs/CONTRIBUTING.md TODO.md
%{_bindir}/*
%{_datadir}/*


%changelog
