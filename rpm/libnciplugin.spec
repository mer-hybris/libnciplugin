Name: libnciplugin

Version: 1.1.4
Release: 0
Summary: Support library for NCI-based nfcd plugins
License: BSD
URL: https://github.com/mer-hybris/libnciplugin
Source: %{name}-%{version}.tar.bz2

%define nfcd_version 1.1.4
%define libncicore_version 1.1.13
%define libglibutil_version 1.0.31

BuildRequires: pkgconfig
BuildRequires: pkgconfig(glib-2.0)
BuildRequires: pkgconfig(libglibutil) >= %{libglibutil_version}
BuildRequires: pkgconfig(libncicore) >= %{libncicore_version}
BuildRequires: pkgconfig(nfcd-plugin) >= %{nfcd_version}

# license macro requires rpm >= 4.11
BuildRequires: pkgconfig(rpm)
%define license_support %(pkg-config --exists 'rpm >= 4.11'; echo $?)

Requires: libglibutil >= %{libglibutil_version}
Requires: libncicore >= %{libncicore_version}
Requires: nfcd >= %{nfcd_version}
Requires(post): /sbin/ldconfig
Requires(postun): /sbin/ldconfig

%description
Provides basic functionality for NCI-based nfcd plugins.

%package devel
Summary: Development library for %{name}
Requires: %{name} = %{version}

%description devel
This package contains the development library for %{name}.

%prep
%setup -q

%build
make %{_smp_mflags} LIBDIR=%{_libdir} KEEP_SYMBOLS=1 release pkgconfig

%install
make LIBDIR=%{_libdir} DESTDIR=%{buildroot} install-dev

%post -p /sbin/ldconfig

%postun -p /sbin/ldconfig

%files
%defattr(-,root,root,-)
%{_libdir}/%{name}.so.*
%if %{license_support} == 0
%license LICENSE
%endif

%files devel
%defattr(-,root,root,-)
%dir %{_includedir}/nciplugin
%{_libdir}/pkgconfig/*.pc
%{_libdir}/%{name}.so
%{_includedir}/nciplugin/*.h
