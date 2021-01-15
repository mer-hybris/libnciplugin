Name: libnciplugin
Version: 1.1.0
Release: 0
Summary: Support library for NCI-based nfcd plugins
License: BSD
URL: https://github.com/mer-hybris/libnciplugin
Source: %{name}-%{version}.tar.bz2

%define nfcd_version 1.1.0
%define libncicore_version 1.1.13
%define libglibutil_version 1.0.31

BuildRequires: pkgconfig(glib-2.0)
BuildRequires: pkgconfig(libglibutil) >= %{libglibutil_version}
BuildRequires: pkgconfig(libncicore) >= %{libncicore_version}
BuildRequires: pkgconfig(nfcd-plugin) >= %{nfcd_version}

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
Requires: pkgconfig

%description devel
This package contains the development library for %{name}.

%prep
%setup -q

%build
make LIBDIR=%{_libdir} KEEP_SYMBOLS=1 release pkgconfig

%install
rm -rf %{buildroot}
make LIBDIR=%{_libdir} DESTDIR=%{buildroot} install-dev

%post -p /sbin/ldconfig

%postun -p /sbin/ldconfig

%files
%defattr(-,root,root,-)
%{_libdir}/%{name}.so.*

%files devel
%defattr(-,root,root,-)
%{_libdir}/pkgconfig/*.pc
%{_libdir}/%{name}.so
%{_includedir}/nciplugin/*.h
