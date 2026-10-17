"""Offline MSVC build/test. Never opens a device, installs, elevates or starts RF."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import shutil

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--mutations', action='store_true')
    parser.add_argument('--vc-root', type=Path, required=True, help='MSVC version directory containing include and lib/x64')
    parser.add_argument('--sdk-root', type=Path, required=True, help='Windows Kits/10 directory')
    parser.add_argument('--sdk-version', default='10.0.19041.0')
    args = parser.parse_args()
    source = Path(__file__).resolve().parent
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    vc = args.vc_root.resolve()
    sdk = args.sdk_root.resolve()
    version = args.sdk_version
    compiler = vc / 'bin/Hostx64/x64/cl.exe'
    env = os.environ.copy()
    env['VSLANG'] = '1033'
    env['PATH'] = str(compiler.parent) + os.pathsep + env['PATH']
    env['INCLUDE'] = os.pathsep.join(map(str, [vc/'include'] + [sdk/'Include'/version/d for d in ('ucrt','shared','um')]))
    env['LIB'] = os.pathsep.join(map(str, [vc/'lib/x64'] + [sdk/'Lib'/version/d/'x64' for d in ('ucrt','um')]))
    files = ['CMakeLists.txt','build_mt7925.py','winawdl_mt7925.cpp','mt7925_codec.h','mt7925_device.h','mt7925_bridge.h','mt7925_rx_diagnostics.h','mt7925_shutdown.h','mt7925_txpower.h','tests/mt7925_txpower_test.cpp','pipe_server.h','radio_link.h','frame_capture.h','tests/frame_capture_test.cpp','tests/mt7925_bridge_test.cpp','tests/pipe_server_test.cpp']
    files += ['mt7925_txs.h','tests/mt7925_txs_test.cpp']
    hashes = {f:hashlib.sha256((source/f).read_bytes()).hexdigest() for f in files}
    report = {'source_sha256':hashes,'compiler':str(compiler),'sdk':version,'commands':[], 'hardware_access':False}
    def run(cmd, cwd):
        result = subprocess.run(list(map(str,cmd)), cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=90)
        report['commands'].append({'argv':list(map(str,cmd)),'exit_code':result.returncode,'output':result.stdout.decode('utf-8','replace')})
        print(report['commands'][-1]['output'],flush=True)
        if result.returncode:raise RuntimeError('build/test command failed')
    try:
        with tempfile.TemporaryDirectory(prefix='mt7925-usermode-') as temp:
            for filename in ('winawdl_mt7925.cpp','tests/mt7925_bridge_test.cpp','tests/pipe_server_test.cpp','tests/frame_capture_test.cpp','tests/mt7925_txpower_test.cpp','tests/mt7925_txs_test.cpp'):
                target=out/(Path(filename).stem+'.exe')
                run([compiler,'/nologo','/std:c++20','/EHsc','/W4','/WX','/MT','/utf-8','/I'+str(source),'/Fe:'+str(target),'/Fo:'+str(Path(temp)/(target.stem+'.obj')),source/filename,'/link','setupapi.lib','bcrypt.lib'],temp)
                run([target,'--help'] if filename=='winawdl_mt7925.cpp' else [target],temp)
            if args.mutations:
                mutations = [
                    ('source-identity','mt7925_bridge.h','if(!identity_ || !std::equal(source_.begin(),source_.end(),input.begin()+26))','if(false)'),
                    ('channel-confirmation','mt7925_bridge.h','if(r.primary!=ch || r.center!=ch || r.width!=20)','if(false)'),
                    ('bssid-filter','mt7925_bridge.h','!std::equal(bssid_.begin(),bssid_.end(),frame.begin()+16)','std::equal(bssid_.begin(),bssid_.end(),frame.begin()+16)'),
                    ('reply-ready','mt7925_codec.h','if(r.ready && (!r.attempted','if(!r.ready && (!r.attempted'),
                    ('firmware-hash','mt7925_device.h','if(actual!=expected)','if(actual!=expected && false)'),
                    ('link-reserved','mt7925_bridge.h','if(h.reserved || h.length>RADIO_LINK_MAX_PAYLOAD ||','if(h.length>RADIO_LINK_MAX_PAYLOAD ||'),
                    ('firmware-address','mt7925_bridge.h','retry(Address,address_request(p),-3);',';'),
                    ('fixed-trial-channel','mt7925_bridge.h','if(fixed_channel_ && ch!=primary_)','if(fixed_channel_ && false)'),
                ]
                outcomes=[]
                for name,file,old,new in mutations:
                    copy=Path(temp)/name;copy.mkdir()
                    for f in files:
                        target=copy/f;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(source/f,target)
                    target=copy/file;code=target.read_text(encoding='utf-8')
                    if code.count(old)!=1:raise RuntimeError('mutation anchor mismatch '+name)
                    target.write_text(code.replace(old,new),encoding='utf-8')
                    binary=copy/'test.exe'
                    cmd=[compiler,'/nologo','/std:c++20','/EHsc','/W4','/WX','/MT','/utf-8','/I'+str(copy),'/Fe:'+str(binary),'/Fo:'+str(copy/'test.obj'),copy/'tests/mt7925_bridge_test.cpp','/link','setupapi.lib','bcrypt.lib']
                    compiled=subprocess.run(list(map(str,cmd)),cwd=copy,env=env,capture_output=True,timeout=60)
                    result={'name':name,'compile_exit':compiled.returncode,'compile_output':compiled.stdout.decode('utf-8','replace')+compiled.stderr.decode('utf-8','replace')}
                    if compiled.returncode:result['outcome']='unviable'
                    else:
                        try:
                            tested=subprocess.run([str(binary)],cwd=copy,capture_output=True,timeout=20)
                            result.update(exit_code=tested.returncode,output=tested.stdout.decode('utf-8','replace'))
                            result['outcome']='caught' if tested.returncode==1 and 'FAIL ' in result['output'] else ('missed' if tested.returncode==0 else 'unviable')
                        except subprocess.TimeoutExpired:result['outcome']='timeout'
                    outcomes.append(result);print(name,result['outcome'],flush=True)
                report['mutations']=outcomes
                report['mutation_counts']={k:sum(r['outcome']==k for r in outcomes) for k in ('caught','missed','unviable','timeout')}
                if any(r['outcome']!='caught' for r in outcomes):raise RuntimeError('mutation requires triage')
        if hashes != {f:hashlib.sha256((source/f).read_bytes()).hexdigest() for f in files}:raise RuntimeError('source changed during verification')
        report['artifacts']={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in out.glob('*.exe')}
        report['success']=True
    finally:
        (out/'verification.json').write_text(json.dumps(report,indent=2),encoding='utf-8')

if __name__=='__main__':main()
