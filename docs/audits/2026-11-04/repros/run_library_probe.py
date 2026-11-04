import json,os,resource,subprocess
from pathlib import Path
root=Path(__file__).resolve().parent
folder=root/'library-home/ytcui';folder.mkdir(parents=True,exist_ok=True)
file=folder/'library.json'
file.write_text(json.dumps([dict(id='old-item',title='Preserve this bookmark',type='video')]))
before=file.read_bytes()
def limit():
 resource.setrlimit(resource.RLIMIT_CORE,(0,0))
 resource.setrlimit(resource.RLIMIT_FSIZE,(256,256))
r=subprocess.run([str(root/'library_probe')],env=dict(os.environ,XDG_DATA_HOME=str(root/'library-home')),
 capture_output=True,timeout=10,preexec_fn=limit)
after=file.read_bytes()
try:json.loads(after);valid=True
except ValueError:valid=False
print('exit:',r.returncode,'before_bytes:',len(before),'after_bytes:',len(after),'valid_json_after_failed_save:',valid)
assert not valid and after!=before
