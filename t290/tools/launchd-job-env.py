#!/usr/bin/env python3
"""launchd-job-env.py IN OUT LABEL_SUBSTR [KEY=VALUE ...]
Show a job of the iPadOS launchd job cache (/System/Library/xpc/launchd.plist)
and set EnvironmentVariables on it (KEY= removes KEY), keeping the file's
format. With no KEY=VALUE only prints the job (OUT is not written)."""
import plistlib
import sys

src, dst, pat = sys.argv[1:4]
sets = sys.argv[4:]
raw = open(src, 'rb').read()
fmt = plistlib.FMT_BINARY if raw.startswith(b'bplist') else plistlib.FMT_XML
d = plistlib.loads(raw)
jobs = [(k, v) for k, v in d['LaunchDaemons'].items() if pat in k]
if len(jobs) != 1:
    raise SystemExit(f'{len(jobs)} jobs match {pat!r}: {[k for k, _ in jobs]}')
path, job = jobs[0]
print(path)
for k, v in job.items():
    if k not in ('MachServices',):
        print(f'  {k}: {v!r}'[:300])
if not sets:
    raise SystemExit(0)
env = job.setdefault('EnvironmentVariables', {})
for s in sets:
    k, v = s.split('=', 1)
    if v:
        env[k] = v
    else:
        env.pop(k, None)
open(dst, 'wb').write(plistlib.dumps(d, fmt=fmt, sort_keys=False))
print(f'{dst}: EnvironmentVariables = {env}')
