# Build through packaging/scripts/package-fedora.sh so both versions agree.
%{!?branch_truce_version:%{error:branch_truce_version is required}}
%{!?branch_truce_native_version:%{error:branch_truce_native_version is required}}
%global app_id com.canpicasoft.branch-truce

Name:           branch-truce
Version:        %{branch_truce_native_version}
Release:        1%{?dist}
Summary:        Offline Git merge and rebase conflict resolver for KDE
License:        GPL-3.0-or-later AND CC0-1.0
URL:            https://github.com/e0ipso/branch-truce
Source0:        %{name}-%{version}.tar.gz
ExclusiveArch:  x86_64

BuildRequires:  cmake >= 3.28
BuildRequires:  gcc-c++
BuildRequires:  make
BuildRequires:  redhat-rpm-config
BuildRequires:  extra-cmake-modules >= 6.0
BuildRequires:  git >= 2.43
BuildRequires:  cmake(Qt6Core) >= 6.6
BuildRequires:  cmake(Qt6Gui)
BuildRequires:  cmake(Qt6Widgets)
BuildRequires:  cmake(Qt6Network)
BuildRequires:  cmake(Qt6Qml)
BuildRequires:  cmake(Qt6Quick)
BuildRequires:  cmake(Qt6QuickControls2)
BuildRequires:  cmake(KF6Kirigami) >= 6.0
BuildRequires:  cmake(KF6SyntaxHighlighting) >= 6.0

# RPM generates native shared-library requirements. QML imports, styles and
# dynamically loaded image/platform plugins need explicit requirements.
Requires:       git >= 2.43
Requires:       hicolor-icon-theme
Requires:       breeze-icon-theme
Requires:       plasma-breeze-qt6
Requires:       qt6-qtsvg%{?_isa} >= 6.6
Requires:       qt6-qtwayland%{?_isa} >= 6.6
Requires:       qt6qml(org.kde.desktop)
Requires:       qt6qml(org.kde.kirigami)
Requires:       qt6qml(QtQml)
Requires:       qt6qml(QtQml.Models)
Requires:       qt6qml(QtQml.WorkerScript)
Requires:       qt6qml(QtQuick)
Requires:       qt6qml(QtQuick.Controls)
Requires:       qt6qml(QtQuick.Layouts)
Requires:       qt6qml(QtQuick.Shapes)
Requires:       qt6qml(QtQuick.Templates)
Requires:       qt6qml(QtQuick.Window)
Recommends:     plasma-integration

%description
Branch Truce opens the conflicted text files of a Git merge or rebase in one
session, shows both contributions around an editable result, and preserves
unfinished work. Built for Fedora 44 x86_64 with native Qt and KDE libraries.

%prep
%autosetup

%build
# Fedora's CMake macros supply the distribution compiler/linker hardening.
# The source's --only-bytecode QML setting avoids private Qt ABI linkage.
%cmake -DBRANCH_TRUCE_VERSION=%{branch_truce_version} -DBRANCH_TRUCE_BUILD_DESKTOP=ON -DBRANCH_TRUCE_WARNINGS_AS_ERRORS=ON -DBUILD_TESTING=OFF
%cmake_build

%install
%cmake_install

%files
%license LICENSE
%{_bindir}/branch-truce
%{_datadir}/applications/%{app_id}.desktop
%{_datadir}/metainfo/%{app_id}.metainfo.xml
%{_datadir}/icons/hicolor/scalable/apps/%{app_id}.svg
%{_datadir}/icons/hicolor/16x16/apps/%{app_id}.png
%{_datadir}/icons/hicolor/22x22/apps/%{app_id}.png
%{_datadir}/icons/hicolor/24x24/apps/%{app_id}.png
%{_datadir}/icons/hicolor/32x32/apps/%{app_id}.png
