import os
from pathlib import Path
import subprocess
root=Path(__file__).resolve().parent
repo=Path(os.environ.get('AUDIT_REPO', str(root.parent/'ytcui-test-repo')))
flags=['g++','-w','-std=c++17','-O1','-g','-fsanitize=address','-fno-omit-frame-pointer','-fno-access-control',
       '-DUSE_YTCUIDL','-DYTUI_VERSION="'+(repo/'VERSION').read_text().strip()+'"','-DYTCUI_VERSION="'+(repo/'VERSION').read_text().strip()+'"',
       '-I'+str(repo/'include'),'-I'+str(repo/'src'),'-I'+str(repo/'third_party/ytcui-dl/include')]
with (root/'uaf-build.log').open('w') as log:
    subprocess.run(flags+['-c',str(repo/'src/app.cpp'),'-o',str(root/'app_asan.o')],stdout=log,stderr=log,check=True)
    objects=[str(p) for p in (repo/'build-audit/CMakeFiles/ytcui.dir').rglob('*.o')
             if not str(p).endswith(('/src/main.cpp.o','/src/app.cpp.o'))]
    subprocess.run(flags+[str(root/'app_uaf_probe.cpp'),str(root/'app_asan.o'),*objects,
                         '-Wl,--wrap=_ZN5ytcui2sb5fetchERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEES8_i',
                         '-lssl','-lcrypto','-lz','-lncursesw','-ltinfo','-pthread','-o',str(root/'app_uaf_probe')],
                   stdout=log,stderr=log,check=True)
env=dict(os.environ,XDG_CONFIG_HOME=str(root/'sb-home/config'),XDG_DATA_HOME=str(root/'sb-home/data'),
         XDG_CACHE_HOME=str(root/'sb-home/cache'),ASAN_OPTIONS='detect_leaks=0:halt_on_error=1:symbolize=0')
result=subprocess.run([str(root/'app_uaf_probe')],env=env,capture_output=True,timeout=20)
(root/'app-uaf.log').write_bytes(result.stdout+result.stderr)
print('exit:',result.returncode)
for line in result.stderr.decode(errors='replace').splitlines():
    if 'heap-use-after-free' in line or 'src/app.cpp:' in line: print(line)
