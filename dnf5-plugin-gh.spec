Name:           dnf5-plugin-gh
Version:        0.1.1
Release:        1%{?dist}
Summary:        DNF5 plugin to install and update RPMs from GitHub releases

# FIXME: confirm the license once a LICENSE file is added to the project
License:        GPL-3.0-or-later
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  cmake
BuildRequires:  gcc-c++
BuildRequires:  pkgconfig(libdnf5)
BuildRequires:  pkgconfig(libdnf5-cli)
BuildRequires:  dnf5-devel
BuildRequires:  libcurl-devel
BuildRequires:  json-devel

Requires:       dnf5%{?_isa}
Requires:       createrepo_c

%description
A dnf5 plugin that installs and updates RPM packages published as GitHub
release assets. Each tracked project is mirrored into a local repository, so
dnf5 handles dependency resolution, upgrades and removal natively.

Adds the gh-install, gh-update, gh-info and gh-remove commands.

%prep
%autosetup

%build
%cmake
%cmake_build

%check
%ctest

%install
%cmake_install
install -d %{buildroot}%{_sharedstatedir}/dnf5-gh

%files
%doc README.md
%{_libdir}/dnf5/plugins/gh_cmd_plugin.so
%{_libdir}/libdnf5/plugins/gh.so
%config(noreplace) %{_sysconfdir}/dnf/libdnf5-plugins/gh.conf
%dir %{_sharedstatedir}/dnf5-gh

%changelog
* Mon Nov 09 2026 dnf5-plugin-gh contributors - 0.1.1-1
- Stop tracking GitHub packages removed through dnf

* Mon Nov 09 2026 dnf5-plugin-gh contributors - 0.1.0-2
- Refresh GitHub repositories automatically during updates

* Mon Nov 09 2026 LuanVSO <luanvso@users.noreply.github.com> - 0.1.0-1
- Initial package
