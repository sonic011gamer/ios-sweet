# Local bridge build on this Windows PC

The bridge and `eagltest` can be compiled and ad-hoc signed inside WSL Ubuntu
22.04. No macOS runner is needed for this build. This does not run the macOS
fake-GPU tests, or prove that pixels render correctly on the tablet.

Installed tools and caches are outside the repository:

- `/usr/bin/clang` (Ubuntu LLVM 14), `ld64.lld-14`, `libplist-dev`.
- `/home/kirill/t290-toolchain/iPhoneOS16.5.sdk`, from Theos SDK release
  `master-146e41f`. Archive SHA256:
  `5e0fd3f01266cce4ce012d4a99b38eb56578fca40d09edc81cd83dee958202fb`.
- `/home/kirill/t290-toolchain/ldid-src/ldid`, built from ProcursusTeam/ldid
  commit `af86971ae72ec3ed3d0a699107c4e882324c941b`.
- `/home/kirill/t290-toolchain/gli-layout.txt`, the saved 975-slot SDK layout
  from `D:\test1\dl\gli\gli-layout.txt`. The generator rejects a changed slot
  count, non-contiguous indices or the wrong `finish` slot.
- `/home/kirill/mesa-src/mesa-24.2.8/include/GL`, additional GL declarations
  for mapping the desktop GLI names onto Mesa entry points.

From PowerShell:

```powershell
wsl -d Ubuntu-22.04 -- bash /mnt/d/test1/ios-sweet/t290/eagl/build-local.sh
```

Default output: `/home/kirill/t290-toolchain/build-eagl/`.
An optional first argument selects another output directory. Tool locations
can be overridden with `T290_TOOLCHAIN`, `T290_IOS_SDK`, `T290_GLI_LAYOUT`,
`T290_GL_HEADERS`, `CC`, `LD64` and `LDID`.

Outputs include `t290eagl.dylib`, `eagltest`, `SHA256SUMS`, `build-info.txt`
and the generated source. `verify-local.py` checks baseline ARM64 (not arm64e),
iOS 16.0 deployment, each SHA256 code-signature page and the test's signed
entitlements. The script does not write to the tablet.

The installed `t290launch --test` guard must still run `eagltest` on the real
GPU before enabling the bridge in backboardd. A failure keeps the system
renderer active and records the failing pixel test in pstore.

For disappearing-content diagnosis, a file containing `1` at
`/private/var/tmp/t290eagl.trace` enables bounded tracing in backboardd:
shader sources (49 KiB budget), program/shader associations, uniform names,
and sampled draw/texture state. It does not enable tracing in `eagltest`.
Write `0` to the marker and restart the daemon/boot to disable it.
Trace-enabled performance is not a benchmark. The USB viewer collects the
kernel log while requesting full native-panel frames.

Full XNU kernel builds are still performed in the macOS workflow. Local kernel
single-file compilation uses the existing cached generated headers in WSL;
the complete Darwin kernel build/link toolchain has not been ported here.
Mesa itself is also still built by the existing macOS workflow.

Upstream setup references: [Theos Linux/WSL](https://theos.dev/docs/installation-linux),
[SDK release](https://github.com/theos/sdks/releases/tag/master-146e41f),
[ldid source](https://github.com/ProcursusTeam/ldid),
[Mach-O LLD](https://lld.llvm.org/MachO/index.html).
