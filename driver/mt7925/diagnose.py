"""Query an installed candidate; never install or elevate.

--probe reads a fixed set of registers. --preflight validates the pinned firmware
inside the driver without downloading it. Neither option starts DMA or a radio.
--platform reads only fixed MTDS/MTGS ACPI methods and records exact status/data;
it does not claim their SAR semantics or radio readiness have been validated.
--boot-diagnostic explicitly opts into a one-shot firmware/DMA test and stop;
it requires separate hardware authorization and the pinned firmware folder.
"""
import argparse
import ctypes as C
from ctypes import wintypes as W
import hashlib
import json
from pathlib import Path
import struct
import uuid

GUID='{3c9e8b97-7d9b-4db2-913d-da4279250001}'

class Interface(C.Structure):
    _fields_=[('cbSize',W.DWORD),('guid',C.c_byte*16),('flags',W.DWORD),('reserved',C.c_size_t)]

def find_device():
    api=C.WinDLL('setupapi',use_last_error=True)
    api.SetupDiGetClassDevsW.argtypes=[C.c_void_p,W.LPCWSTR,W.HWND,W.DWORD]
    api.SetupDiGetClassDevsW.restype=W.HANDLE
    api.SetupDiEnumDeviceInterfaces.argtypes=[W.HANDLE,C.c_void_p,C.c_void_p,W.DWORD,C.POINTER(Interface)]
    api.SetupDiGetDeviceInterfaceDetailW.argtypes=[W.HANDLE,C.POINTER(Interface),C.c_void_p,W.DWORD,
                                                   C.POINTER(W.DWORD),C.c_void_p]
    api.SetupDiDestroyDeviceInfoList.argtypes=[W.HANDLE]
    guid=C.create_string_buffer(uuid.UUID(GUID).bytes_le)
    handle=api.SetupDiGetClassDevsW(guid,None,None,0x12)
    if handle==C.c_void_p(-1).value:raise C.WinError(C.get_last_error())
    paths=[]
    try:
        for index in range(16):
            entry=Interface();entry.cbSize=C.sizeof(entry)
            if not api.SetupDiEnumDeviceInterfaces(handle,None,guid,index,C.byref(entry)):
                if C.get_last_error()==259:break
                raise C.WinError(C.get_last_error())
            needed=W.DWORD()
            api.SetupDiGetDeviceInterfaceDetailW(handle,C.byref(entry),None,0,C.byref(needed),None)
            if needed.value<8 or needed.value>65536:raise RuntimeError('Invalid interface detail size')
            buf=C.create_string_buffer(needed.value)
            C.cast(buf,C.POINTER(W.DWORD))[0]=8 if C.sizeof(C.c_void_p)==8 else 6
            if not api.SetupDiGetDeviceInterfaceDetailW(handle,C.byref(entry),buf,needed,None,None):
                raise C.WinError(C.get_last_error())
            paths.append(C.wstring_at(C.addressof(buf)+4))
    finally:api.SetupDiDestroyDeviceInfoList(handle)
    if len(paths)!=1:raise RuntimeError(f'Expected one installed MT7925 candidate, found {len(paths)}')
    return paths[0]

def firmware_bundle(folder):
    manifest=json.loads((folder/'manifest.json').read_text())
    expected={item['name']:item['sha256'] for item in manifest}
    images=[]
    for name in ('WIFI_MT7925_PATCH_MCU_1_1_hdr.bin','WIFI_RAM_CODE_MT7925_1_1.bin'):
        path=folder/name
        if path.stat().st_size>8*1024**2:raise RuntimeError('Firmware too large')
        data=path.read_bytes()
        if hashlib.sha256(data).hexdigest()!=expected[name]:raise RuntimeError('Firmware hash mismatch')
        images.append(data)
    return struct.pack('<4I',0x3546574d,1,*map(len,images))+b''.join(images)

def decode_platform(data):
    if len(data)!=248:raise RuntimeError('Unexpected platform response size')
    size,version,error,reserved=struct.unpack_from('<IIiI',data)
    if (size,version,reserved)!=(248,1,0):raise RuntimeError('Unexpected platform response ABI')
    result=dict(size=size,version=version,error=error)
    for name,offset in [('dynamic',16),('geographic',132)]:
        state,status,count=struct.unpack_from('<III',data,offset)
        payload=data[offset+12:offset+116]
        if state not in (0,1,2) or count>104 or any(payload[count:]):
            raise RuntimeError('Malformed platform table output')
        if (state==0 and count) or (state==1 and (status!=0xc0000034 or count)) or (state==2 and (status or count<4)):
            raise RuntimeError('Inconsistent platform table status')
        if not error and state==0:raise RuntimeError('Unknown platform table reported as success')
        result[name]=dict(state={0:'unknown',1:'absent',2:'data'}[state],
            ntstatus=f'0x{status:08x}',bytes=count,data_hex=payload[:count].hex())
    return result

def query(probe=False,firmware=None,boot=False,platform=False):
    if boot and firmware is None:raise ValueError('Boot diagnostic requires pinned firmware')
    bundle=firmware_bundle(firmware) if firmware else None
    path=find_device()
    api=C.WinDLL('kernel32',use_last_error=True)
    api.CreateFileW.argtypes=[W.LPCWSTR,W.DWORD,W.DWORD,C.c_void_p,W.DWORD,W.DWORD,W.HANDLE]
    api.CreateFileW.restype=W.HANDLE
    api.DeviceIoControl.argtypes=[W.HANDLE,W.DWORD,C.c_void_p,W.DWORD,C.c_void_p,W.DWORD,
                                  C.POINTER(W.DWORD),C.c_void_p]
    api.CloseHandle.argtypes=[W.HANDLE]
    handle=api.CreateFileW(path,0xc0000000,0,None,3,0,None)
    if handle==C.c_void_p(-1).value:raise C.WinError(C.get_last_error())
    def raw_control(number,access,capacity,data=b''):
        output=C.create_string_buffer(capacity);returned=W.DWORD()
        code=(0x22<<16)|(access<<14)|(number<<2)
        if not api.DeviceIoControl(handle,code,data if data else None,len(data),output,len(output),C.byref(returned),None):
            raise C.WinError(C.get_last_error())
        if returned.value!=len(output):
            raise RuntimeError(f'IOCTL {number:#x}: returned {returned.value} bytes, expected {len(output)}')
        return output.raw
    def control(number,access,fields,data=b'',version=1):
        raw=raw_control(number,access,4*len(fields),data)
        result=dict(zip(fields,struct.unpack('<'+'I'*len(fields),raw)))
        if result['size']!=len(raw) or result['version']!=version:raise RuntimeError('Unexpected response ABI')
        return result
    try:
        result={'info':control(0x800,1,['size','version','vendor','device','bar_bytes','dma_address_bits',
                                      'mcu_entries','fwdl_entries','buffer_bytes','prepared','radio_ready','reserved'])}
        if (result['info']['vendor'],result['info']['device'])!=(0x14c3,0x7925):
            raise RuntimeError('Unexpected device identity')
        if platform:
            result['platform']=decode_platform(raw_control(0x804,1,248))
            result['platform_query_validated']=result['platform']['error']==0
        if probe:
            result['probe']=control(0x801,1,['size','version','ownership','firmware_state','dma_config',
                'interrupt_mask','command_cpu','command_dma','download_cpu','download_dma','rx_cpu','rx_dma'])
            result['registers_accessible']=all(result['probe'][n]!=0xffffffff for n in
                                              ('ownership','firmware_state','dma_config'))
        if firmware:
            result['firmware_preflight']=control(0x802,3,['size','version','patch_regions','ram_regions',
                'download_regions','download_bytes','entry','reserved'],bundle)
        if boot:
            result['firmware_boot']=control(0x803,3,['size','version','patch_present','regions_sent',
                'bytes_sent','start_acked','ready','firmware_error','release_error','primary_error',
                'stop_error','published','quiescent','stage','last_command','last_state','snapshot_valid',
                'snapshot_ownership','snapshot_firmware_state','snapshot_dma_config',
                'snapshot_command_cpu','snapshot_command_dma','snapshot_download_cpu','snapshot_download_dma',
                'snapshot_rx_cpu','snapshot_rx_dma','snapshot_mac_pm'],bundle,version=2)
            for key in ('firmware_error','release_error','primary_error','stop_error'):
                result['firmware_boot'][key]=C.c_int32(result['firmware_boot'][key]).value
            b=result['firmware_boot']
            b['stage_name']={0:'not_started',1:'tx_space',2:'doorbell',3:'reply',4:'download_drain',
                5:'rom_state',6:'running_state'}.get(b['stage'],'unknown')
            result['firmware_boot_validated']=(b['primary_error']==b['stop_error']==0 and
                b['ready']==b['start_acked']==b['published']==b['quiescent']==1)
        return result
    finally:api.CloseHandle(handle)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--probe',action='store_true')
    p.add_argument('--preflight',type=Path,metavar='FIRMWARE_FOLDER')
    p.add_argument('--boot-diagnostic',action='store_true')
    p.add_argument('--platform',action='store_true')
    p.add_argument('--output',type=Path,required=True)
    args=p.parse_args()
    if args.output.exists():raise RuntimeError('Output already exists; preserving it')
    try:
        result=query(args.probe,args.preflight,args.boot_diagnostic,args.platform)
        code=int((args.boot_diagnostic and not result.get('firmware_boot_validated')) or
                 (args.platform and not result.get('platform_query_validated')))
    except (OSError,RuntimeError,ValueError,KeyError) as error:
        result={'error':str(error),'hardware_validated':False};code=1
    args.output.write_text(json.dumps(result,indent=2),encoding='utf-8')
    print(json.dumps(result))
    return code

if __name__=='__main__':raise SystemExit(main())
