"""Build an UNSIGNED, NOT INSTALLED KMDF bring-up candidate in a temporary kit.

Requires a build-manager lease or explicit exception for this driver build.
Downloads pinned Microsoft NuGet development files; never runs an installer.
"""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import urllib.request
import zipfile

VERSION = '10.0.26100.1'
KIT = '10.0.26100.0'
PACKAGES = ('microsoft.windows.wdk.x64', 'microsoft.windows.sdk.cpp.x64',
            'microsoft.windows.sdk.cpp')
# Restrict extraction to headers/libraries required by this x64 KMDF candidate.
PREFIXES = (f'c/include/{KIT}/km/', f'c/include/{KIT}/shared/',
            f'c/include/{KIT}/ucrt/', 'c/include/wdf/kmdf/1.33/',
            f'c/lib/{KIT}/km/x64/', 'c/lib/wdf/kmdf/x64/1.33/')
LIMIT = 2 * 1024**3


def acquire_kit(folder):
    evidence = []
    expanded = 0
    for package in PACKAGES:
        url = f'https://api.nuget.org/v3-flatcontainer/{package}/{VERSION}/{package}.{VERSION}.nupkg'
        archive = folder / (package + '.nupkg')
        digest = hashlib.sha512()
        received = 0
        with urllib.request.urlopen(url, timeout=60) as response, archive.open('wb') as output:
            expected = response.headers.get('x-ms-meta-SHA512')
            while True:
                chunk = response.read(1024 * 1024)
                if not chunk:
                    break
                received += len(chunk)
                if expanded + received > LIMIT:
                    raise RuntimeError('Development kit exceeds authorized 2 GiB limit')
                output.write(chunk)
                digest.update(chunk)
        checksum = base64.b64encode(digest.digest()).decode()
        if not expected or checksum != expected:
            raise RuntimeError(f'NuGet SHA-512 mismatch or missing digest: {package}')
        with zipfile.ZipFile(archive) as z:
            for member in z.infolist():
                normalized = member.filename.lower()
                if member.is_dir() or not (normalized.startswith(PREFIXES) or normalized == 'license.txt'):
                    continue
                target = (folder / member.filename).resolve()
                if not target.is_relative_to(folder.resolve()):
                    raise RuntimeError('Archive path escapes kit directory')
                expanded += member.file_size
                if expanded + received > LIMIT:
                    raise RuntimeError('Expanded kit exceeds authorized 2 GiB limit')
                target.parent.mkdir(parents=True, exist_ok=True)
                with z.open(member) as source, target.open('wb') as output:
                    shutil.copyfileobj(source, output)
        evidence.append(dict(package=package, version=VERSION, bytes=received, sha512=checksum, url=url))
        archive.unlink()
    return evidence


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--artifact-dir', type=Path, required=True)
    parser.add_argument('--linker', choices=('lld', 'msvc'), default='lld',
                        help='Compare linker output without changing driver source')
    parser.add_argument('--compiler', choices=('clang', 'msvc'), default='clang')
    args = parser.parse_args()
    root = Path(__file__).resolve().parent
    artifact = args.artifact_dir.resolve()
    artifact.mkdir(parents=True, exist_ok=True)
    if (artifact / 'mt7925_bringup.sys').exists():
        raise RuntimeError('Choose a new artifact directory; existing binary is preserved')
    clang = shutil.which('clang-cl.exe')
    linker = shutil.which('lld-link.exe')
    if not clang or not linker:
        raise RuntimeError('Existing clang-cl and lld-link are required')
    vswhere = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')) / 'Microsoft Visual Studio/Installer/vswhere.exe'
    found = subprocess.check_output([str(vswhere), '-latest', '-products', '*',
        '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
        '-property', 'installationPath'], text=True).strip()
    if not found:
        raise RuntimeError('MSVC headers not found through vswhere')
    msvc = max((Path(found) / 'VC/Tools/MSVC').iterdir())
    if args.compiler == 'msvc':
        clang = str(msvc / 'bin/Hostx64/x64/cl.exe')
        if not Path(clang).is_file():
            raise RuntimeError('Existing MSVC compiler is unavailable')
    if args.linker == 'msvc':
        linker = str(msvc / 'bin/Hostx64/x64/link.exe')
        if not Path(linker).is_file():
            raise RuntimeError('Existing MSVC linker is unavailable')
    report = dict(stage='started', installed=False, radio_ready=False, packages=[], commands=[],
                  source_sha256={name: hashlib.sha256((root / name).read_bytes()).hexdigest()
                                 for name in ('core.c', 'core.h', 'device.c', 'device.h', 'data_memory.h', 'mcu_memory.h',
                                               'firmware.c', 'firmware.h', 'pcie_mcu.c', 'pcie_mcu.h', 'nic_caps.c', 'nic_caps.h', 'tx_power.c', 'tx_power.h', 'mac_init.c', 'mac_init.h', 'clc_power.c', 'clc_power.h', 'efuse_type.c', 'efuse_type.h', 'regulatory.c', 'regulatory.h', 'channel_control.c', 'channel_control.h', 'raw_tx.c', 'raw_tx.h',
                                              'rx_ring.c', 'rx_ring.h', 'raw_rx.c', 'raw_rx.h', 'mcu_runtime.c', 'mcu_runtime.h',
                                              'chip_control.c', 'chip_control.h', 'tx_init.c', 'tx_init.h', 'platform_acpi.c', 'platform_acpi.h', 'radio_session.c', 'radio_session.h', 'radio_device.c', 'radio_device.h', 'build_driver.py', 'diagnose.py')})
    try:
        with tempfile.TemporaryDirectory(prefix='mt7925-wdk-') as temporary:
            folder = Path(temporary).resolve()
            report['packages'] = acquire_kit(folder)
            includes = [msvc / 'include', folder / f'c/Include/{KIT}/km',
                folder / f'c/Include/{KIT}/shared', folder / f'c/Include/{KIT}/ucrt',
                folder / 'c/Include/wdf/kmdf/1.33']
            def run(command):
                env = dict(os.environ, VSLANG='1033')
                completed = subprocess.run(command, cwd=folder, capture_output=True, env=env)
                raw_output = completed.stdout + completed.stderr
                try:
                    output = raw_output.decode('utf-8')
                except UnicodeDecodeError:
                    output = raw_output.decode('mbcs', errors='replace')
                report['commands'].append(dict(arguments=command, exit=completed.returncode,
                                               output=output,
                                               output_base64=base64.b64encode(completed.stdout + completed.stderr).decode()))
                if completed.returncode:
                    raise RuntimeError(output)
            objects = []
            include_flags = (['/external:W0', *['/external:I' + str(p) for p in includes]]
                             if args.compiler == 'msvc' else ['/imsvc' + str(p) for p in includes])
            for source in ('core.c', 'firmware.c', 'pcie_mcu.c', 'nic_caps.c', 'tx_power.c', 'mac_init.c', 'clc_power.c', 'efuse_type.c', 'regulatory.c', 'channel_control.c', 'tx_init.c', 'platform_acpi.c', 'raw_tx.c', 'rx_ring.c', 'raw_rx.c', 'mcu_runtime.c', 'chip_control.c', 'radio_session.c', 'radio_device.c', 'device.c'):
                obj = folder / (source + '.obj')
                run([clang, '/nologo', '/c', '/W4', '/WX', '/kernel', '/Zl', '/GS',
                    '/guard:cf', '/D_AMD64_', '/D_WIN64', '/D_WIN32_WINNT=0x0A00',
                    '/DNTDDI_VERSION=0x0A00000C', '/DKMDF_VERSION_MAJOR=1', '/DKMDF_VERSION_MINOR=33',
                    # Kit/MSVC headers are external; retain /WX for our source.
                    *include_flags, '/Fo' + str(obj), str(root / source)])
                objects.append(str(obj))
            binary = folder / 'mt7925_bringup.sys'
            run([linker, '/driver:wdm', '/release', '/subsystem:native', '/entry:FxDriverEntry', '/nodefaultlib',
                '/machine:x64', '/integritycheck', '/guard:cf', '/dynamicbase', '/nxcompat',
                '/out:' + str(binary), '/libpath:' + str(folder / f'c/Lib/{KIT}/km/x64'),
                '/libpath:' + str(folder / 'c/Lib/wdf/kmdf/x64/1.33'),
                *objects, 'ntoskrnl.lib', 'hal.lib', 'wmilib.lib', 'WdfDriverEntry.lib',
                'WdfLdr.lib', 'BufferOverflowFastFailK.lib'])
            shutil.copy2(binary, artifact / binary.name)
            report['sha256'] = hashlib.sha256(binary.read_bytes()).hexdigest()
            report['stage'] = 'linked_unsigned_not_installed'
    except Exception as error:
        report['stage'] = 'failed'
        report['error'] = str(error)
        raise
    finally:
        (artifact / 'build-report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps({'stage': report['stage'], 'sha256': report['sha256']}))


if __name__ == '__main__':
    main()
