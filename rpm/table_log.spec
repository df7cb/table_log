# rpmbuild -D'pgmajorversion 14' -D'pginstdir /usr/pgsql-14' -ba rpm/table_log.spec

%global debug_package %{nil}
%global sname table_log

%if 0%{?rhel} && 0%{?rhel} == 7
%ifarch ppc64 ppc64le
%pgdg_set_ppc64le_compiler_at10
%endif
%endif

Summary:	PostgreSQL table log extension
Name:		%{sname}_%{pgmajorversion}
Version:	0.6.3
Release:	1%{?dist}
License:	BSD
Source0:	https://github.com/df7cb/table_log/archive/v%{version}.tar.gz
URL:		https://github.com/df7cb/table_log/
BuildRequires:	postgresql%{pgmajorversion}-devel pgdg-srpm-macros
Requires:	postgresql%{pgmajorversion}-server

%if 0%{?rhel} && 0%{?rhel} == 7
%ifarch ppc64 ppc64le
%pgdg_set_ppc64le_min_requires
%endif
%endif

%description
table_log is a PostgreSQL extension with a set of functions to log changes on
a table and to restore the state of the table or a specific row on any time in
the past.

%prep
%setup -q -n table_log-%{version}

%build
%if 0%{?rhel} && 0%{?rhel} == 7
%ifarch ppc64 ppc64le
	%pgdg_set_ppc64le_compiler_flags
%endif
%endif

USE_PGXS=1 PATH=%{pginstdir}/bin/:$PATH %{__make} %{?_smp_mflags}

%install
%{__rm} -rf %{buildroot}
USE_PGXS=1 PATH=%{pginstdir}/bin/:$PATH %{__make} DESTDIR=%{buildroot} %{?_smp_mflags} install
%{__install} -d %{buildroot}%{pginstdir}/share/extension
%{__install} -d %{buildroot}%{pginstdir}/bin

%clean
%{__rm} -rf %{buildroot}

%files
%defattr(644,root,root,755)
%doc %{pginstdir}/doc/extension/table_log.md
%{pginstdir}/lib/table_log*
%{pginstdir}/lib/bitcode/table_log*
%{pginstdir}/share/extension/table_log*.sql*
%{pginstdir}/share/extension/table_log.control

%changelog
* Thu Jan 20 2022 Christoph Berg <myon@debian.org> - 0.6.3-1
- Initial packaging for PostgreSQL RPM Repository

