#!/usr/bin/env python3
"""launchd-job-args.py IN OUT LABEL_SUBSTR ARG...
Replace ProgramArguments of one job in the iPadOS launchd job cache
(/System/Library/xpc/launchd.plist), keeping the file's format; also drops
EnvironmentVariables.DYLD_INSERT_LIBRARIES (t290launch sets it instead)."""
import plistlib
import sys

src, dst, pat = sys.argv[1:4]
args = sys.argv[4:]
raw = open(src, 'rb').read()
fmt = plistlib.FMT_BINARY if raw.startswith(b'bplist') else plistlib.FMT_XML
d = plistlib.loads(raw)
jobs = [(k, v) for k, v in d['LaunchDaemons'].items() if pat in k]
if len(jobs) != 1:
    raise SystemExit(f'{len(jobs)} jobs match {pat!r}: {[k for k, _ in jobs]}')
path, job = jobs[0]
print(f'{path}: {job.get("ProgramArguments")} -> {args}')
job['ProgramArguments'] = args
env = job.get('EnvironmentVariables')
if env is not None:
    env.pop('DYLD_INSERT_LIBRARIES', None)
    if not env:
        del job['EnvironmentVariables']
open(dst, 'wb').write(plistlib.dumps(d, fmt=fmt, sort_keys=False))
print(f'{dst} written')
