Name:             rspamd
Version:          3.2
Release:          1
Summary:          Rapid spam filtering system
Group:            System Environment/Daemons
License:          Apache-2.0
URL:              https://rspamd.com
Source0:          https://github.com/rspamd/rspamd/archive/%{version}/%{name}-%{version}.tar.gz
Source1:          %{name}.logrotate
Source2:          80-rspamd.preset
BuildRoot:        %{_tmppath}/%{name}-%{version}-%{release}
%if 0%{?el7}
BuildRequires:    cmake3
BuildRequires:    devtoolset-8-gcc-c++
%else
BuildRequires:    cmake
BuildRequires:    gcc-c++
%endif
BuildRequires:    file-devel
BuildRequires:    glib2-devel
BuildRequires:    lapack-devel
BuildRequires:    libevent-devel
BuildRequires:    libicu-devel
BuildRequires:    libsodium-devel
BuildRequires:    libunwind-devel
%ifarch x86_64 amd64
BuildRequires:    hyperscan-devel
BuildRequires:    jemalloc-devel
BuildRequires:    luajit-devel
%else
BuildRequires:    lua-devel
%endif
%ifarch arm64 aarch64
BuildRequires:    vectorscan-devel
%endif
BuildRequires:    openblas-devel
BuildRequires:    openssl-devel
BuildRequires:    pcre2-devel
BuildRequires:    ragel
BuildRequires:    sqlite-devel
BuildRequires:    systemd
Requires(pre):    shadow-utils
Requires(post):   systemd
Requires(preun):  systemd
Requires(postun): systemd

%description
Rspamd is a rapid, modular and lightweight spam filter. It is designed to work
with big amount of mail and can be easily extended with own filters written in
lua.

%prep
%setup -q -n rspamd-%{version}

%build
%if 0%{?el7}
%{__cmake3} \
%else
%{__cmake} \
%endif
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_C_FLAGS_RELEASE="%{optflags}" \
        -DCMAKE_CXX_FLAGS_RELEASE="%{optflags}" \
        -DENABLE_LTO=ON \
%if 0%{?fedora} >= 36
        -DLINKER_NAME=/usr/bin/ld.bfd \
%endif
        -DCMAKE_INSTALL_PREFIX=%{_prefix} \
        -DCONFDIR=%{_sysconfdir}/rspamd \
        -DMANDIR=%{_mandir} \
        -DDBDIR=%{_localstatedir}/lib/rspamd \
        -DRUNDIR=%{_localstatedir}/run/rspamd \
        -DLOGDIR=%{_localstatedir}/log/rspamd \
        -DEXAMPLESDIR=%{_datadir}/examples/rspamd \
        -DSHAREDIR=%{_datadir}/rspamd \
        -DLIBDIR=%{_libdir}/rspamd/ \
        -DINCLUDEDIR=%{_includedir} \
        -DRSPAMD_GROUP=_rspamd \
        -DRSPAMD_USER=_rspamd \
        -DSYSTEMDDIR=%{_unitdir} \
        -DWANT_SYSTEMD_UNITS=ON \
        -DNO_SHARED=ON \
        -DDEBIAN_BUILD=1 \
        -DENABLE_LIBUNWIND=ON \
%ifarch x86_64 amd64 arm64 aarch64
        -DENABLE_HYPERSCAN=ON \
%endif
%ifarch x86_64 amd64
        -DENABLE_JEMALLOC=ON \
        -DENABLE_LUAJIT=ON \
%endif
        -DENABLE_BLAS=ON

%{__make} %{?_smp_mflags}

%install
%{__make} install DESTDIR=%{buildroot} INSTALLDIRS=vendor
%{__install} -p -D -m 0644 %{SOURCE1} %{buildroot}%{_sysconfdir}/logrotate.d/%{name}
%{__install} -p -D -m 0644 %{SOURCE2} %{buildroot}%{_presetdir}/80-rspamd.preset
%{__install} -d -p -m 0755 %{buildroot}%{_localstatedir}/log/rspamd
%{__install} -d -p -m 0755 %{buildroot}%{_localstatedir}/lib/rspamd
%{__install} -p -D -d -m 0755 %{buildroot}%{_sysconfdir}/%{name}/local.d/
%{__install} -p -D -d -m 0755 %{buildroot}%{_sysconfdir}/%{name}/override.d/

%clean
rm -rf %{buildroot}

%pre
%{_sbindir}/groupadd -r _rspamd 2>/dev/null || :
%{_sbindir}/useradd -g _rspamd -c "Rspamd user" -s /bin/false -r -d %{_localstatedir}/lib/rspamd _rspamd 2>/dev/null || :

%post
%{__chown} -R _rspamd:_rspamd %{_localstatedir}/lib/rspamd
%{__chown} _rspamd:_rspamd %{_localstatedir}/log/rspamd
systemctl --no-reload preset %{name}.service >/dev/null 2>&1 || :

%preun
%systemd_preun %{name}.service

%postun
%systemd_postun_with_restart %{name}.service

%files
%defattr(-,root,root,-)

%dir %{_sysconfdir}/rspamd
%config(noreplace) %{_sysconfdir}/rspamd/*

%{_bindir}/rspamd
%{_bindir}/rspamd_stats
%{_bindir}/rspamc
%{_bindir}/rspamadm

%{_unitdir}/%{name}.service
%{_presetdir}/80-rspamd.preset

%dir %{_libdir}/rspamd
%{_libdir}/rspamd/*

%{_mandir}/man8/%{name}.*
%{_mandir}/man1/rspamc.*
%{_mandir}/man1/rspamadm.*

%dir %{_datadir}/rspamd
%{_datadir}/rspamd/*

%config(noreplace) %{_sysconfdir}/logrotate.d/%{name}

%attr(-, _rspamd, _rspamd) %dir %{_localstatedir}/lib/rspamd
%dir %{_localstatedir}/log/rspamd