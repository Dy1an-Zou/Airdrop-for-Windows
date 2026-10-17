# Installer backend

`setup-backend.ps1` runs with the Windows built-in **64-bit Windows PowerShell 5.1**. The front end owns the ordinary UI and UAC prompt. The backend never elevates itself, starts the product, stops a process, installs a driver, or changes signing/security settings.

## Invocation contract

All modes take `-ExpectedManifestSha256`, the SHA-256 of the payload's exact `runtime-manifest.json` embedded at build time. Return code 0 means success; 1 means failure. One JSON object is written to standard output and, if supplied, a **new** `-ResultPath` file. Never reuse a result filename or treat a missing result as success.

| Mode | Other required arguments | Elevation | Success status |
| --- | --- | --- | --- |
| `verify-payload` | `-PackageDir` | No | `verified` |
| `preflight` | `-PackageDir`, `-ExpectedOwnerSid`, `-InstallerPath` | No | `ready` |
| `install` | Same as preflight | Yes | `installed` or `updated` |
| `uninstall-preflight` | `-ExpectedOwnerSid` | No | `ready` |
| `uninstall` | `-ExpectedOwnerSid` | Yes | `uninstalled` |

`ExpectedOwnerSid` must come from the original, unelevated installer user. That same account must be an administrator; switching to a different administrator at UAC is rejected. Uninstall also binds the current user to the installed receipt. `AutoStart` accepts `preserve` (default), `enabled`, or `disabled`. Preserve retains an existing startup preference and enables it on a fresh install.

`InstallerPath` identifies the current local Setup executable. The backend copies and hashes it into `%CommonProgramFiles%\AirdropForWindowsSetup\Setup.exe`, alongside a separate `setup-receipt.json`, and owns the matching HKLM Apps uninstall entry. The front end must not separately overwrite those files or register a duplicate uninstall entry. They remain outside the product manifest directory.

Failures have `status: failed`, `mode`, and `error` (stable error code prefix plus readable message). Some messages include a short Chinese translation; the front end supplies the main localized presentation.

## Scope and recovery

Install/preflight require Windows x64 build 26100+, .NET Desktop Runtime 10 x64, and exactly one currently present device with hardware ID `PCI\VEN_14C3&DEV_7925&SUBSYS_60001A3B&REV_00`, service `mt7925_bringup`, status OK/problem 0, and the pinned experimental driver SHA-256 `938148d69b14c911c18c4c7a9ef00703ad5625e46d876143b8f887df6762aafb`. There is no machine-specific instance ID. Uninstall does not require the hardware or .NET runtime to remain present.

The payload must be version 0.2.3. Updates accept only the verified 0.2.2 manifest `234f8a9655769cc036ddec42493b5bc69dd8d41d06c5640f65bd0fe777d2b067`, or a repair of the same exact new payload. Receiver, Python, native binaries, firmware, and approved artwork must remain identical, except for the one paired example-name cleanup whose exact old/new hashes are pinned in `Assert-FrozenComponents` (`receiver/receive_trial.py` and its `receiver/manifest.json`). Both changes must match together; other changes remain forbidden. Complete file hashes, receipt identity, path containment, reparse-point exclusion, and unknown-file/directory rejection apply before replacement or deletion.

The app and helper must already have exited normally. The backend holds the product mutexes during changes, stages under protected Program Files directories, checks the copied content, swaps directories, and rolls back if installation fails. Successful updates remove only their verified old copies. Cleanup failures leave an exact backup path and `cleanupWarning`; they do not falsely roll back an otherwise completed install.

Uninstall retains user configuration, received files, ProgramData logs, driver installation and security policy. It removes only verified product files, using individual literal-path deletes and empty-directory removal, never a recursive delete of unknown content. If the running Setup executable cannot be removed, `setupCleanupPending: true` and `setupFolder` identify the remaining uninstaller stub. The front end should tell the user it can be deleted after Setup exits; do not force-kill processes or schedule broad deferred deletion.

## Offline verification

`check-backend.ps1` parses the actual backend and imports only its function definitions, never the executable entrypoint. It creates tiny fixtures under the current TEMP directory, exercises manifest/hash/path and frozen-component failures, and injects startup-write and helper-registration failures through the real staging/swap/rollback function with OS registration, ACL, process, and mutex dependencies replaced by fixture mocks. It also exercises a successful update/uninstall and rejects unknown content before cleanup. It performs no real install/uninstall, hardware access, or process shutdown. Fixtures are retained for inspection or managed-temp cleanup. Run it through the user's approved build/test workflow. A passing fixture result is implementation evidence, not real installation acceptance. `check-backend-mutations.ps1` applies four separate mutants under TEMP (manifest digest, unknown-file refusal, protected-runtime digest, rollback restoration) and reports caught/missed/unviable rather than treating a harness error as a caught defect.
