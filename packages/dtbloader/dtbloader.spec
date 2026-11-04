%global debug_package %{nil}
%global source_date_epoch_from_changelog 0

Name:           dtbloader
Version:        0
Release:        2%{?dist}.armada
Summary:        EFI driver that installs a devicetree for the running device

# dtbloader itself is BSD-3-Clause but it bundles projects with other licenses.
License:        BSD-3-Clause AND (GPL-2.0-or-later OR BSD-2-Clause) AND BSD-2-Clause AND LicenseRef-Fedora-Public-Domain
URL:            https://github.com/TravMurav/dtbloader
Source0:        %{url}/releases/download/%{version}/dtbloader-%{version}.tar.gz

ExclusiveArch:  aarch64
BuildRequires:  clang
BuildRequires:  lld
BuildRequires:  llvm
BuildRequires:  make

%description
dtbloader picks the devicetree for the running device and installs it in the
UEFI configuration table, for platforms whose firmware does not provide one.

%prep
%autosetup

%build
# On a device it does not know (e.g. one whose firmware already passes a
# devicetree), return EFI_ABORTED instead of EFI_UNSUPPORTED: systemd-boot
# then skips the red "Failed to start image" error and its 2.5 s pause.
%make_build ARCH=aarch64 ABORT_IF_UNSUPPORTED=1

%install
install -Dpm 0644 build-aarch64/dtbloader.efi %{buildroot}%{_prefix}/lib/dtbloader/dtbloaderaa64.efi

%files
%license LICENSE
%doc README.md
%{_prefix}/lib/dtbloader/dtbloaderaa64.efi

%changelog
