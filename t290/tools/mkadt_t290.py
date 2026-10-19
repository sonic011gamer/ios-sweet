#!/usr/bin/env python3
"""Apple Device Tree for the SWEET kernel built with SWEET_BOARD=t290.

Starts from tools/mkadt.py (chosen, defaults, identity, wdt, disp0, ...) and
replaces what differs on the Galaxy Tab A 8.0 2019 (SM-T290, SDM429):
GICv2 instead of GICv3, four Cortex-A53, the RAM window and the splash
framebuffer. Usage:  mkadt_t290.py OUT.adt [--qemu] [--dump]

--qemu: QEMU virt with -M virt,gic-version=2 -cpu cortex-a53 (t290/tools/qemu-t290.sh).
Hardware addresses: msm8937.dtsi / sdm429-cpu.dtsi of LineageOS
android_kernel_samsung_sdm429 (see t290/NOTES.md); not yet checked against the
tablet's live FDT.
"""
import os, sys

TOOLS = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../tools')
sys.path.insert(0, TOOLS)
import adt  # noqa: E402
import mkadt  # noqa: E402
from mkadt import u32, u64, cstr, SOC_BASE  # noqa: E402

# QGIC2: GICD and the memory-mapped CPU interface (GICC)
GICD, GICD_SIZE = 0x0b000000, 0x1000
GICC, GICC_SIZE = 0x0b002000, 0x1000
VTIMER_INTID = 20             # timer <1 4 ...> = PPI 4
TIMEBASE = 19200000
NCPUS = 4                     # Cortex-A53, MPIDR 0x100..0x103 (Aff1 = 1)
CPU_HZ = 1958400000
# RAM above every reserved-memory carve-out of msm8937.dtsi (highest:
# ss_plog 0x92200000 + 4M) and the splash buffer, up to the 2 GiB bank end.
PHYS_BASE, MEM_SIZE = 0xa0000000, 0x60000000
# cont_splash_mem: 0x90000000, 0x1400000; 800x1280 panel, 32 bpp assumed
FB, FB_SIZE = 0x90000000, 0x1400000
FB_W, FB_H = 800, 1280

QEMU_GICD, QEMU_GICC, QEMU_TIMEBASE = 0x08000000, 0x08010000, 62500000
QEMU_VTIMER_INTID = 27


def set_props(n, **kv):
    """Replace (or append) properties; values are bytes."""
    for key, value in kv.items():
        key = key.replace('_', '-')
        for i, (k, f, _) in enumerate(n.props):
            if k == key:
                n.props[i] = (k, f, value)
                break
        else:
            n.props.append((key, 0, value))


def child(n, name):
    for c in n.children:
        if c.name == name:
            return c
    raise SystemExit(f'mkadt.py tree has no {name!r} node')


def build(qemu=False):
    root = mkadt.build(qemu=qemu)
    gicd, gicc, timebase, vtimer = ((QEMU_GICD, QEMU_GICC, QEMU_TIMEBASE, QEMU_VTIMER_INTID)
                                    if qemu else (GICD, GICC, TIMEBASE, VTIMER_INTID))

    set_props(child(root, 'chosen'), dram_base=u64(PHYS_BASE), dram_size=u64(MEM_SIZE),
              firmware_version=cstr('t290-shim'), system_firmware_version=cstr('t290-shim'))

    gic = child(child(root, 'arm-io'), 'gic')
    set_props(gic, compatible=cstr('arm,gic-400') + cstr('arm,cortex-a7-gic'),
              virtual_timer_intid=u32(vtimer),
              reg=u64(gicd - SOC_BASE, GICD_SIZE, gicc - SOC_BASE, GICC_SIZE))

    # CPUs: keep mkadt's property set, change identity, MPIDR and count.
    cpus = child(root, 'cpus')
    template = cpus.children[0]
    cpus.children = []
    for core in range(1 if qemu else NCPUS):
        c = adt.Node()
        c.props = list(template.props)
        set_props(c, name=cstr(f'cpu{core}'),
                  compatible=cstr('arm,cortex-a53') + cstr('ARM,v8'),
                  reg=u32(core if qemu else 0x100 | core),
                  cpu_id=u32(core), cluster_core_id=u32(core),
                  l2_cache_id=u32(0), l3_cache_id=u32(0),
                  state=cstr('running' if core == 0 else 'stopped'),
                  timebase_frequency=u32(timebase), peripheral_frequency=u32(timebase),
                  fixed_frequency=u32(timebase), clock_frequency=u32(CPU_HZ))
        cpus.children.append(c)

    set_props(child(root, 'sweet-fbcon'), reg=u64(FB, FB_SIZE), width=u32(FB_W),
              height=u32(FB_H), stride=u32(FB_W * 4))

    # sweet's UFS node points at SM7150 MMIO; the tablet has eMMC (SDHCI).
    root.children = [c for c in root.children if c.name != 'sweet-ufs']

    compatible = b''.join(v for k, _, v in root.props if k == 'compatible')
    set_props(root, compatible=compatible.replace(b'qcom,sm7150\0', b'samsung,gtowifi\0qcom,sdm429\0'))
    return root


if __name__ == '__main__':
    root = build(qemu='--qemu' in sys.argv)
    blob = adt.serialize(root)
    open(sys.argv[1], 'wb').write(blob)
    if '--dump' in sys.argv:
        adt.dump(adt.parse(blob)[0])
    print(f'{sys.argv[1]}: {len(blob)} bytes', file=sys.stderr)
