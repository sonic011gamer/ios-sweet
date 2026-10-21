#!/usr/bin/env python3
"""launchd-add-job.py IN OUT LABEL PROGRAM [ARG...]
Add a run-once root job to the iPadOS launchd job cache (/System/Library/xpc/
launchd.plist, read as is with launchd_unsecure_cache=1), keeping the file's
format (binary or XML). Output goes to /dev/console (kernel log, pstore)."""
import plistlib
import sys

src, dst, label, prog = sys.argv[1:5]
raw = open(src, 'rb').read()
fmt = plistlib.FMT_BINARY if raw.startswith(b'bplist') else plistlib.FMT_XML
d = plistlib.loads(raw)
d['LaunchDaemons'][f'/System/Library/LaunchDaemons/{label}.plist'] = {
    'Label': label,
    'ProgramArguments': [prog] + sys.argv[5:],
    'RunAtLoad': True,
    'KeepAlive': False,
    'LaunchOnlyOnce': True,
    'UserName': 'root',
    # the console lands in the kernel log, which the pstore mirror keeps
    'StandardOutPath': '/dev/console',
    'StandardErrorPath': '/dev/console',
}
open(dst, 'wb').write(plistlib.dumps(d, fmt=fmt, sort_keys=False))
print(f'{dst}: {"binary" if fmt == plistlib.FMT_BINARY else "xml"}, {len(d["LaunchDaemons"])} daemons, added {label}')
