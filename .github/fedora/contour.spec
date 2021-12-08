# This enables cmake to work consistently between pre-f33 and after.
# https://docs.fedoraproject.org/en-US/packaging-guidelines/CMake/#_notes
%undefine __cmake_in_source_build

Name:           contour
Version:        0.3.0
Release:        1%{?dist}
Summary:        Modern C++ Terminal Emulator

License:        ASL 2.0
URL:            https://github.com/contour-terminal/%{name}
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  cmake
BuildRequires:  extra-cmake-modules
BuildRequires:  ninja-build
BuildRequires:  gcc-c++
BuildRequires:  pkgconf
BuildRequires:  freetype-devel
BuildRequires:  harfbuzz-devel
BuildRequires:  qt5-qtbase-devel
BuildRequires:  qt5-qtbase-gui
BuildRequires:  kf5-kwindowsystem-devel

%description
contour is a modern terminal emulator, for everyday use.
It is aiming for power users with a modern feature mindset.


%prep
%setup -q -n %{name}-%{version}


%build
BUILD_TYPE=Release ./autogen.sh
%ninja_build


%install
%ninja_install
# Create needed directories
mkdir -p %{buildroot}%{_bindir}
mkdir -p %{buildroot}%{_datadir}/applications
mkdir -p %{buildroot}%{_datadir}/terminfo
mkdir -p %{buildroot}%{_datadir}/pixmaps
mkdir -p %{buildroot}%{_datadir}/contour
# Move /usr/local files to /usr to follow build standards
mv %{buildroot}/usr/local/bin/%{name} %{buildroot}%{_bindir}/%{name}
for _dir in $(ls "%{buildroot}/usr/local/share"); do
    # Moved directories:
    # - applications
    # - terminfo
    # - pixmaps
    # - contour
    mv "%{buildroot}/usr/local/share/$_dir" "%{buildroot}%{_datadir}"
done
# Remove non-needed /usr/local directory
rm -rf %{buildroot}/usr/local
# verify desktop file
desktop-file-validate %{buildroot}%{_datadir}/applications/%{name}.desktop


%check


%files
%license LICENSE.txt
%doc README.md Changelog.md CONTRIBUTING.md TODO.md
%{_bindir}/*
%{_datadir}/*


%changelog
* Sun Dec 05 2021 NTBBloodbath <bloodbathalchemist@protonmail.com> 0.3.0-1
- Initial RPM package
