# SPDX-License-Identifier: MIT
"""Query Windows' actual radio APIs before choosing an AWDL transport.

The default operation is read-only. --try-monitor is an explicit, reversible
mode trial on one disconnected adapter, and requires an elevated process.
--try-packet-monitor uses the installed Packet driver's own access permissions.
No driver installation, packet capture, transmission, or elevation is automatic.
"""
import argparse
import ctypes as C
import json
import os
from pathlib import Path
import struct
import sys
import uuid

DWORD = C.c_uint32
HANDLE = C.c_void_p
MONITOR = 0x80000000
CURRENT_OPERATION_MODE = 12
OID_MODE_CAPABILITY = 0x0D010307
OID_CURRENT_MODE = 0x0D010308
OID_MEDIA_IN_USE = 0x00010104


def decode_mode_capability(raw):
    if len(raw) != 24:
        raise ValueError(f'operation-mode capability must be 24 bytes, got {len(raw)}')
    reserved, major, minor, tx, rx, mask = struct.unpack('<6I', raw)
    return {'reserved': reserved, 'version': [major, minor], 'tx_buffers': tx,
            'rx_buffers': rx, 'mask': f'0x{mask:08x}', 'monitor': bool(mask & MONITOR)}


def try_monitor(api, guid, *, connected, elevated):
    """A successful setter is insufficient: query it and restore the old mode."""
    result = {'monitor_observed': False}
    if connected:
        return {**result, 'blocked': 'interface_connected'}
    if not elevated:
        return {**result, 'blocked': 'administrator_required'}
    before = api.mode(guid)
    result['before'] = before
    if before['error'] or 'value' not in before:
        return {**result, 'blocked': 'original_mode_unknown'}
    result['set_error'] = api.set_mode(guid, MONITOR)
    after = None
    try:
        after = api.mode(guid)
        result['after'] = after
        result['monitor_observed'] = after.get('value') == MONITOR and not after['error']
    except OSError as error:
        result['trial_error'] = str(error)
    finally:
        if not after or after.get('error') or after.get('value') != before['value']:
            result['restore_error'] = api.set_mode(guid, before['value'])
        else:
            result['restore_error'] = 0
        try:
            final = api.mode(guid)
            result['final'] = final
            result['restored'] = not final.get('error') and final.get('value') == before['value']
        except OSError as error:
            result['restored'] = False
            result['restore_query_error'] = str(error)
    return result


def try_packet_monitor(api, handle, *, disconnected):
    """Trial the standard DOT11 mode OID; verify and undo even a partial failure."""
    result = {'monitor_observed': False, 'restored': False}
    before = api.query(handle, OID_CURRENT_MODE, 8)
    result['before'] = before
    if not before.get('ok') or len(bytes.fromhex(before.get('hex', ''))) != 8:
        return {**result, 'blocked': 'original_mode_unknown'}
    original = bytes.fromhex(before['hex'])
    if not disconnected():
        return {**result, 'blocked': 'interface_not_disconnected'}
    after = None
    try:
        result['set'] = api.set(handle, OID_CURRENT_MODE, struct.pack('<II', 0, MONITOR))
        after = api.query(handle, OID_CURRENT_MODE, 8)
        result['after'] = after
        result['monitor_observed'] = (after.get('ok') is True
            and after.get('hex') == struct.pack('<II', 0, MONITOR).hex())
    except OSError as error:
        result['trial_error'] = str(error)
    finally:
        # A failing setter can still have changed state. Query before deciding
        # whether a restore is necessary; unknown state also requires restore.
        if not after or not after.get('ok') or after.get('hex') != original.hex():
            try:
                result['restore'] = api.set(handle, OID_CURRENT_MODE, original)
                final = api.query(handle, OID_CURRENT_MODE, 8)
                result['final'] = final
                result['restored'] = final.get('ok') is True and final.get('hex') == original.hex()
            except OSError as error:
                result['restore_error'] = str(error)
        else:
            result['restored'] = True
    return result


class GUID(C.Structure):
    _fields_ = [('data', C.c_ubyte * 16)]

    @classmethod
    def from_text(cls, text):
        return cls.from_buffer_copy(uuid.UUID(text.strip('{}')).bytes_le)

    def __str__(self):
        return '{' + str(uuid.UUID(bytes_le=bytes(self.data))) + '}'


class InterfaceInfo(C.Structure):
    _fields_ = [('guid', GUID), ('description', C.c_wchar * 256), ('state', DWORD)]


class NativeWlan:
    def __init__(self):
        self.dll = C.WinDLL('wlanapi.dll', use_last_error=True)
        functions = {
            'WlanOpenHandle': ([DWORD, HANDLE, C.POINTER(DWORD), C.POINTER(HANDLE)], DWORD),
            'WlanCloseHandle': ([HANDLE, HANDLE], DWORD),
            'WlanEnumInterfaces': ([HANDLE, HANDLE, C.POINTER(HANDLE)], DWORD),
            'WlanFreeMemory': ([HANDLE], None),
            'WlanQueryInterface': ([HANDLE, C.POINTER(GUID), DWORD, HANDLE,
                                   C.POINTER(DWORD), C.POINTER(HANDLE), C.POINTER(DWORD)], DWORD),
            'WlanSetInterface': ([HANDLE, C.POINTER(GUID), DWORD, DWORD, HANDLE, HANDLE], DWORD),
        }
        for name, (args, restype) in functions.items():
            function = getattr(self.dll, name)
            function.argtypes, function.restype = args, restype
        self.handle = HANDLE()
        negotiated = DWORD()
        error = self.dll.WlanOpenHandle(2, None, C.byref(negotiated), C.byref(self.handle))
        if error:
            raise OSError(error, C.FormatError(error))

    def close(self):
        if self.handle:
            self.dll.WlanCloseHandle(self.handle, None)
            self.handle = HANDLE()

    def interfaces(self):
        pointer = HANDLE()
        error = self.dll.WlanEnumInterfaces(self.handle, None, C.byref(pointer))
        if error:
            raise OSError(error, C.FormatError(error))
        try:
            count = DWORD.from_address(pointer.value).value
            if count > 256:
                raise ValueError('implausible WLAN interface count')
            rows = []
            for index in range(count):
                info = InterfaceInfo.from_address(pointer.value + 8 + index * C.sizeof(InterfaceInfo))
                rows.append({'guid': str(info.guid), 'description': info.description,
                             'state': info.state, 'connected': info.state == 1})
            return rows
        finally:
            self.dll.WlanFreeMemory(pointer)

    def mode(self, guid):
        key, size, data, source = GUID.from_text(guid), DWORD(), HANDLE(), DWORD()
        error = self.dll.WlanQueryInterface(self.handle, C.byref(key), CURRENT_OPERATION_MODE,
                                            None, C.byref(size), C.byref(data), C.byref(source))
        if error:
            return {'error': error, 'message': C.FormatError(error).strip()}
        try:
            if size.value != 4:
                return {'error': 13, 'message': 'invalid DWORD mode response', 'bytes': size.value}
            return {'error': 0, 'value': DWORD.from_address(data.value).value}
        finally:
            self.dll.WlanFreeMemory(data)

    def set_mode(self, guid, mode):
        key, value = GUID.from_text(guid), DWORD(mode)
        return self.dll.WlanSetInterface(self.handle, C.byref(key), CURRENT_OPERATION_MODE,
                                         C.sizeof(value), C.byref(value), None)


class PacketQueries:
    """Standard OIDs only. Ethernet support is never treated as raw Wi-Fi."""
    def __init__(self, path):
        self.path = str(path)
        self.dll = C.WinDLL(self.path, use_last_error=True)
        self.dll.PacketOpenAdapter.argtypes = [C.c_char_p]
        self.dll.PacketOpenAdapter.restype = HANDLE
        self.dll.PacketCloseAdapter.argtypes = [HANDLE]
        self.dll.PacketCloseAdapter.restype = None
        self.dll.PacketRequest.argtypes = [HANDLE, C.c_ubyte, HANDLE]
        self.dll.PacketRequest.restype = C.c_ubyte
        self.dll.PacketGetVersion.argtypes = []
        self.dll.PacketGetVersion.restype = C.c_char_p

    def query(self, handle, oid, size):
        buffer = C.create_string_buffer(struct.pack('<II', oid, size) + bytes(size))
        C.set_last_error(0)
        # FALSE: this is a query, not an OID setter.
        ok = self.dll.PacketRequest(handle, 0, C.byref(buffer))
        if not ok:
            error = C.get_last_error()
            return {'ok': False, 'error': error, 'message': C.FormatError(error).strip()}
        length = struct.unpack_from('<I', buffer.raw, 4)[0]
        if length > size:
            return {'ok': False, 'error': 13, 'message': 'driver returned oversized OID data'}
        return {'ok': True, 'hex': buffer.raw[8:8 + length].hex()}

    def set(self, handle, oid, data):
        buffer = C.create_string_buffer(struct.pack('<II', oid, len(data)) + data)
        C.set_last_error(0)
        ok = self.dll.PacketRequest(handle, 1, C.byref(buffer))
        error = 0 if ok else C.get_last_error() & 0xffffffff
        return {'ok': bool(ok), 'error': error,
                'error_hex': f'0x{error:08x}',
                'message': C.FormatError(error).strip() if error else ''}

    def probe(self, guid, disconnected=None):
        name = ('\\Device\\NPF_' + guid).encode('ascii')
        C.set_last_error(0)
        handle = self.dll.PacketOpenAdapter(name)
        result = {'dll': self.path, 'version': self.dll.PacketGetVersion().decode('ascii', 'replace')}
        if not handle:
            return {**result, 'open_error': C.get_last_error()}
        try:
            result['media'] = self.query(handle, OID_MEDIA_IN_USE, 4)
            result['current_mode'] = self.query(handle, OID_CURRENT_MODE, 8)
            result['capability'] = self.query(handle, OID_MODE_CAPABILITY, 24)
            cap = result['capability']
            if cap['ok']:
                try:
                    result['decoded_capability'] = decode_mode_capability(bytes.fromhex(cap['hex']))
                except ValueError as error:
                    result['decode_error'] = str(error)
            if disconnected is not None:
                result['monitor_trial'] = try_packet_monitor(self, handle,
                                                              disconnected=disconnected)
            return result
        finally:
            self.dll.PacketCloseAdapter(handle)


def packet_libraries():
    system = Path(os.environ.get('SystemRoot', r'C:\Windows')) / 'System32'
    return [path for path in (system / 'Npcap' / 'Packet.dll', system / 'Packet.dll')
            if path.is_file()]


def is_admin():
    dll = C.WinDLL('shell32.dll')
    dll.IsUserAnAdmin.argtypes = []
    dll.IsUserAnAdmin.restype = C.c_int
    return bool(dll.IsUserAnAdmin())


def collect(guid=None, monitor_trial=False, packet_trial=False):
    if sys.platform != 'win32':
        raise OSError('this probe uses Windows Native Wi-Fi APIs')
    api = NativeWlan()
    report = {'schema': 'winawdl.windows-radio.v1', 'elevated': is_admin(),
              'scope': 'local_adapter_only', 'interfaces': [],
              'airdrop_transfer_verified': False}
    try:
        libraries = []
        report['library_errors'] = []
        for path in packet_libraries():
            try:
                libraries.append(PacketQueries(path))
            except (OSError, AttributeError) as error:
                report['library_errors'].append({'path': str(path), 'error': str(error)})
        for interface in api.interfaces():
            if guid and uuid.UUID(guid.strip('{}')) != uuid.UUID(interface['guid'].strip('{}')):
                continue
            interface['native_mode'] = api.mode(interface['guid'])
            def disconnected():
                return any(row['guid'] == interface['guid'] and row['state'] == 4
                           for row in api.interfaces())
            interface['packet_queries'] = [library.probe(interface['guid'],
                disconnected if packet_trial else None) for library in libraries]
            if packet_trial:
                interface['native_mode_after_packet_trial'] = api.mode(interface['guid'])
            if monitor_trial:
                interface['monitor_trial'] = try_monitor(
                    api, interface['guid'], connected=interface['state'] != 4,
                    elevated=report['elevated'])
            report['interfaces'].append(interface)
        if guid and not report['interfaces']:
            raise ValueError('requested WLAN adapter was not found')
        return report
    finally:
        api.close()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--adapter-guid', help='select exactly one Windows WLAN adapter')
    parser.add_argument('--try-monitor', action='store_true',
                        help='try monitor mode, verify it, and restore; needs administrator rights')
    parser.add_argument('--try-packet-monitor', action='store_true',
                        help='trial the standard mode OID via the installed Packet driver and restore')
    parser.add_argument('--output', type=Path, help='save the diagnostic JSON')
    args = parser.parse_args(argv)
    if (args.try_monitor or args.try_packet_monitor) and not args.adapter_guid:
        parser.error('mode trials require --adapter-guid')
    try:
        report = collect(args.adapter_guid, args.try_monitor, args.try_packet_monitor)
    except (OSError, ValueError) as error:
        print(json.dumps({'error': str(error)}, ensure_ascii=False))
        return 2
    text = json.dumps(report, ensure_ascii=False, indent=2)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text + '\n', encoding='utf-8')
    print(text)
    # Exit zero means diagnostics collected, never that AirDrop is operational.
    return 0


if __name__ == '__main__':
    sys.exit(main())
