# RTL8852CE 0.2.4 candidate validation

Hardware acceptance: **NOT RUN**. No RTL8852CE is available to the author.
Discovery, transfers, signal strength, shutdown, restart and sleep / wake require
a community tester with the dedicated adapter. Upstream reports are not this
project's hardware acceptance.

## Offline implementation checks

2026-10-17, Windows x64, .NET SDK 10.0.402:

- Desktop app compiled with 0 warnings and 0 errors.
- Backend checks: 63 passed, including source / backend mismatch, wrong PCI
  device and driver identity, driver image tampering, signature-only changes,
  malformed PE ranges, and shutdown failure gates.
- Existing helper checks: 49 passed.
- Installer profile checks: 30 passed against the actual pinned unsigned upstream
  driver file; the DLL was read as data and never loaded.
- Existing installer fixture checks: 39 passed, including rollback, unknown-file
  preservation, and user-data preservation. These are isolated fixtures, not a live install.
- Existing language checks passed using isolated settings; user settings and radio
  were not touched.

Seven directed mutations were caught (4 C#, 3 PowerShell): removing source binding,
driver identity, ready-before-clean-stop, or executable-integrity checks. There
were no missed or unviable cases. The C# mutation harness compiles the actual
profile class and offline checks with a constant-only host stub; it does not
exercise the live device boundary.

A realistic Windows full hardware-ID fixture first exposed a false rejection;
both installer and helper now accept the PCI SUBSYS / REV / CC forms while
retaining the exact `10EC:C852` vendor / device match. See Microsoft's
[PCI identifier documentation](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/identifiers-for-pci-devices).
The installer scripts were also checked using Windows PowerShell 5.1, the same
engine used by the installer, and saved with a UTF-8 BOM for Chinese text.

The delivery folder's `VALIDATION.json` records payload and installer verification.
The new candidate has not been installed on the author's MT7925 PC and does not
replace the existing application or the published 0.2.3 release.
