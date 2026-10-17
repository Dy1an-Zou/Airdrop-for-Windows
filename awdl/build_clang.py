"""Build pinned OWL with installed Clang and Windows SDK; no driver/runtime activation."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import shutil

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--out',type=Path,required=True)
    parser.add_argument('--vc-root', type=Path, required=True, help='MSVC version directory containing include and lib/x64')
    parser.add_argument('--sdk-root', type=Path, required=True, help='Windows Kits/10 directory')
    parser.add_argument('--sdk-version', default='10.0.19041.0')
    parser.add_argument('--clang', type=Path, required=True, help='clang.exe; lld must be available beside it or on PATH')
    args=parser.parse_args()
    source=Path(__file__).resolve().parent;repo=source.parent;owl=repo/'upstream/owl'
    out=args.out.resolve();out.mkdir(parents=True,exist_ok=False)
    vc=args.vc_root.resolve()
    sdk=args.sdk_root.resolve();version=args.sdk_version
    compiler=args.clang.resolve()
    env=os.environ.copy();env['PATH']=str(compiler.parent)+os.pathsep+env['PATH']
    env['LIB']=os.pathsep.join(map(str,[vc/'lib/x64',sdk/'Lib'/version/'ucrt/x64',sdk/'Lib'/version/'um/x64']))
    includes=[vc/'include']+[sdk/'Include'/version/d for d in ('ucrt','shared','um')]
    names='log state election sync channel schedule tx rx frame crc32 wire peers version hashmap siphash24 circular_buffer'.split()
    upstream=[owl/'src'/(s+'.c') for s in names]+[owl/'radiotap/radiotap.c',owl/'daemon/core.c']
    windows=[source/'win'/(s+'.c') for s in 'main ev_win io_win netutils_win radio_client redact frame_fixup tcp_merge service_bridge slot_policy'.split()]+[source/'compat/ether.c']
    host=[source/'win/host_if.c',source/'win/host_frame.c',source/'win/posix_win.c']
    tracked=[p for folder in [source/'win',source/'compat',source/'tests',owl/'src',owl/'daemon',owl/'radiotap'] for p in folder.rglob('*') if p.is_file() and p.suffix in ('.c','.h')]
    tracked += [Path(__file__),source/'CMakeLists.txt',repo/'radio/radio_link.h']
    hashes={str(p.relative_to(repo)):hashlib.sha256(p.read_bytes()).hexdigest() for p in tracked}
    report={'source_sha256':hashes,'compiler':str(compiler),'sdk':version,'commands':[],'hardware_access':False}
    def run(cmd,cwd,expected=0):
        r=subprocess.run(list(map(str,cmd)),cwd=cwd,env=env,capture_output=True,timeout=90)
        text=(r.stdout+r.stderr).decode('utf-8','replace')
        report['commands'].append({'argv':list(map(str,cmd)),'exit_code':r.returncode,'expected_exit':expected,'output':text})
        if text:print(text[-2400:],flush=True)
        if r.returncode!=expected:raise RuntimeError('compile/link/offline check failed')
    try:
        with tempfile.TemporaryDirectory(prefix='winawdl-clang-') as tmp:
            temp=Path(tmp);objects={}
            common=[compiler,'-target','x86_64-pc-windows-msvc','-std=gnu11','-fms-extensions','-fms-runtime-lib=static','-D_CRT_SECURE_NO_WARNINGS','-DWIN32_LEAN_AND_MEAN=','-DNOMINMAX=']
            for p in includes:common+=['-isystem',p]
            def compile(p,compat=True,strict=False):
                key=str(p.relative_to(repo));obj=temp/(key.replace('/','_').replace('\\','_')+'.obj')
                cmd=common+['-c',p,'-o',obj,'-I',source/'win']
                if compat:
                    cmd+=['-include',source/'compat/owl_win.h','-mno-ms-bitfields']
                    for inc in [source/'compat/msvc',source/'compat',owl/'src',owl/'radiotap',owl/'daemon',repo/'radio']:cmd+=['-I',inc]
                    cmd+=['-Dlog_log=__real_log_log' if p==owl/'src/log.c' else '-Dlog_log=__wrap_log_log']
                if strict:cmd+=['-Wall','-Wextra','-Werror']
                if p==owl/'daemon/core.c':cmd+=['-Dawdl_is_multicast_eaw=winawdl_is_multicast_eaw','-Dawdl_init_full_action_frame=winawdl_init_full_action_frame']
                run(cmd,temp);objects[p]=obj;return obj
            for p in upstream+windows:compile(p,True,p in windows)
            for p in host:compile(p,False,True)
            def link(name,objs):
                target=out/(name+'.exe')
                libs=[sdk/'Lib'/version/'um/x64'/name for name in ('iphlpapi.lib','bcrypt.lib','ws2_32.lib','advapi32.lib')]
                run([compiler,'-target','x86_64-pc-windows-msvc','-fuse-ld=lld','-fms-runtime-lib=static',*objs,'-o',target,'-Xlinker','/subsystem:console',*libs],temp)
                return target
            exe=link('awdld',[objects[p] for p in upstream+windows+host])
            run([exe,'-c','0'],temp,1) # Reject invalid channel before pipe/adapter access.
            tests={'host_frame':['host_frame'],'tcp_merge':['tcp_merge'],'frame_fixup':['frame_fixup'],'service_bridge':['service_bridge','frame_fixup'],'redact':['redact'],'posix_win':['posix_win']}
            for name,parts in tests.items():
                test=source/'tests'/(name+'_test.c');obj=compile(test,False,True)
                exe=link(name+'_test',[obj]+[objects[source/'win'/(p+'.c')] for p in parts]);run([exe],temp)
            obj=compile(source/'tests/slot_policy_test.c',True,True)
            parts=[objects[owl/'src'/(n+'.c')] for n in names if n!='log']+[objects[owl/'radiotap/radiotap.c'],objects[source/'win/posix_win.c']]
            exe=link('slot_policy_test',[obj,objects[source/'win/slot_policy.c'],objects[source/'compat/ether.c'],*parts]);run([exe],temp)
            obj=compile(source/'tests/intersect_test.c',True,True)
            exe=link('intersect_test',[obj,objects[source/'win/slot_policy.c'],objects[source/'compat/ether.c'],*parts]);run([exe],temp)
            obj=compile(source/'tests/multicast_timing_test.c',True,False)
            exe=link('multicast_timing_test',[obj,objects[source/'win/slot_policy.c'],objects[source/'compat/ether.c'],*parts]);run([exe],temp)
            obj=compile(source/'tests/dualband_frame_test.c',True,False)
            frame_parts=[objects[owl/'src'/(n+'.c')] for n in names if n not in ('log','tx')]+[objects[owl/'radiotap/radiotap.c'],objects[source/'win/posix_win.c']]
            exe=link('dualband_frame_test',[obj,objects[source/'win/slot_policy.c'],objects[source/'win/frame_fixup.c'],objects[source/'compat/ether.c'],*frame_parts]);run([exe],temp)
        if hashes!={str(p.relative_to(repo)):hashlib.sha256(p.read_bytes()).hexdigest() for p in tracked}:raise RuntimeError('source drift')
        report['artifacts']={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in out.glob('*.exe')};report['success']=True
    finally:(out/'verification.json').write_text(json.dumps(report,indent=2),encoding='utf-8')

if __name__=='__main__':main()
