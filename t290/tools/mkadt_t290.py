#!/usr/bin/env python3
"""Apple Device Tree for the SWEET kernel built for MSM8994 (Lumia 950 XL / Cityman).

Starts from tools/mkadt.py and replaces what differs on the MSM8994:
GICv2 bases, eight cores (4x Cortex-A53, 4x Cortex-A57), the RAM window, 
and the splash framebuffer. 
Usage: mkadt_msm8994.py OUT.adt [--qemu [--smp N]] [--fb WxH] [--dump]
"""
import os, sys

TOOLS = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../tools')
sys.path.insert(0, TOOLS)
import adt  # noqa: E402
import mkadt  # noqa: E402
from mkadt import u32, u64, cstr, SOC_BASE  # noqa: E402

# QGIC2: GICD and GICC for MSM8994
GICD, GICD_SIZE = 0xf9000000, 0x1000
GICC, GICC_SIZE = 0xf9002000, 0x1000
VTIMER_INTID = 20             # lk2nd platform/msm8994 irqs.h: INT_QTMR_VIRTUAL_TIMER_EXP = PPI 4
                              # (the QEMU GIC keeps its own separate value below)
TIMEBASE = 19200000           # QCOM standard 19.2MHz XO
NCPUS = 8                     # 4x Cortex-A53 + 4x Cortex-A57
CPU_HZ = 2000000000

# Base RAM and size (3 GiB mapping for Lumia 950 XL)
PHYS_BASE, MEM_SIZE = 0x80000000, 0x40000000

# Splash framebuffer based on the LK relocation: 0x03400000
# Lumia 950 XL resolution: 1440x2560 (1440 * 2560 * 4 = 0x00e10000)
FB, FB_SIZE = 0x03400000, 0x00c00000
FB_W, FB_H = 1080, 1920
FB_BPP, FB_SHIFT = 4, 0       # 32 bpp (XRGB8888), standard linear layout

QEMU_GICD, QEMU_GICC, QEMU_TIMEBASE = 0x08000000, 0x08010000, 62500000
QEMU_VTIMER_INTID = 27        # QEMU virt only
QEMU_VIRTIO_MMIO = 0x0a000000

# /product of the real iPad 5 (J71sAP) device tree
J71SAP_ADT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../research/ipsw-ipad5/j71sap.adt')

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

def build(qemu=False, fb_w=FB_W, fb_h=FB_H, qemu_cpus=1, native=False, seed_file=None):
    root = mkadt.build(qemu=qemu)
    gicd, gicc, timebase, vtimer = ((QEMU_GICD, QEMU_GICC, QEMU_TIMEBASE, QEMU_VTIMER_INTID)
                                    if qemu else (GICD, GICC, TIMEBASE, VTIMER_INTID))

    # Memory and platform metadata mapping
    set_props(child(root, 'chosen'), dram_base=u64(PHYS_BASE), dram_size=u64(MEM_SIZE),
              firmware_version=cstr('msm8994-shim'), system_firmware_version=cstr('msm8994-shim'),
              display_scale=u32(1 if native else 2), chip_id=u32(0x8000))

    gic = child(child(root, 'arm-io'), 'gic')
    set_props(gic, compatible=cstr('arm,gic-400') + cstr('arm,cortex-a7-gic'),
              virtual_timer_intid=u32(vtimer),
              reg=u64(gicd - SOC_BASE, GICD_SIZE, gicc - SOC_BASE, GICC_SIZE))

    # CPUs: Build Big.LITTLE topology
    cpus = child(root, 'cpus')
    template = cpus.children[0]
    cpus.children = []
    
    for core in range(qemu_cpus if qemu else NCPUS):
        # Cluster 0 (A53): MPIDR 0x0..0x3, Cluster 1 (A57): MPIDR 0x100..0x103
        mpidr = core if qemu else (core if core < 4 else 0x100 + (core - 4))
        comp_str = cstr('arm,cortex-a53') if core < 4 else cstr('arm,cortex-a57')
        
        c = adt.Node()
        c.props = list(template.props)
        set_props(c, name=cstr(f'cpu{core}'),
                  compatible=comp_str + cstr('qcom,kryo470') + cstr('ARM,v8'),
                  reg=u32(mpidr),
                  cpu_id=u32(core), cluster_core_id=u32(core % 4),
                  l2_cache_id=u32(1 if core >= 4 else 0), l3_cache_id=u32(0),
                  state=cstr('running' if core == 0 else 'stopped'),
                  timebase_frequency=u32(timebase), peripheral_frequency=u32(timebase),
                  fixed_frequency=u32(timebase), clock_frequency=u32(CPU_HZ))
        cpus.children.append(c)

    if fb_w * fb_h * 4 > FB_SIZE:
        raise SystemExit(f'--fb {fb_w}x{fb_h} does not fit the {FB_SIZE:#x} splash region')
    if qemu:
        set_props(child(root, 'sweet-fbcon'), reg=u64(FB, FB_SIZE), width=u32(fb_w),
                  height=u32(fb_h), stride=u32(fb_w * 4))
    else:
        # Standard XRGB8888 32-bit layout for LA platform UEFI framebuffers
        set_props(child(root, 'sweet-fbcon'), reg=u64(FB, FB_SIZE), width=u32(fb_w),
                  height=u32(fb_h), stride=u32(fb_w * FB_BPP), bpp=u32(FB_BPP),
                  shift=u32(FB_SHIFT))
    set_props(child(child(root, 'arm-io'), 'disp0'), canvas_width=u32(FB_W if native else 1536),
              canvas_height=u32(FB_H if native else 2048))

    buttons = child(root, 'buttons')
    buttons.props = [(k, f, v) for k, f, v in buttons.props
                     if k not in ('home-button-type', 'opposed-power-vol-buttons')]
    set_props(buttons, button_names=cstr('hold') + cstr('menu') + cstr('voldown') + cstr('volup'))

    product = mkadt.d22ap_product(J71SAP_ADT)
    root.children = [c for c in root.children if c.name != 'product']
    if product:
        n = adt.Node()
        n.props = [('name', 0, cstr('product')), ('AAPL,phandle', 0, u32(0x100))] + \
            [(k, 0, v) for k, v in product]
        root.children.append(n)

    root.children = [c for c in root.children if c.name != 'sweet-ufs']
    if qemu:
        vblk = adt.Node()
        vblk.props = [('name', 0, cstr('sweet-vblk')), ('compatible', 0, cstr('virtio,mmio')),
                      ('reg', 0, u64(QEMU_VIRTIO_MMIO, 32 * 0x200))]
        root.children.append(vblk)

    # Append Cityman and MSM8994 targets while preserving iPhone10,3 for XNU match checks
    set_props(root, compatible=cstr('J71sAP') + cstr('iPad6,11') + cstr('xiaomi,sweet') +
              cstr('microsoft,cityman') + cstr('qcom,msm8994') + cstr('AppleARM'),
              model=cstr('iPad6,11'), target_type=cstr('J71s'), target_sub_type=cstr('J71sAP'))

    # Pin /chosen/random-seed from a file so regenerated ADTs are byte-identical
    # to the shipped one (default: the seed extracted from the booted vt20 image).
    if seed_file is None:
        default_seed = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'seed-cityman-vt20.bin')
        if os.path.exists(default_seed):
            seed_file = default_seed
    if seed_file:
        seed = open(seed_file, 'rb').read()
        chosen = child(root, 'chosen')
        for i, (k, f, v) in enumerate(chosen.props):
            if k == 'random-seed':
                chosen.props[i] = (k, f, seed)
                break
    return root


if __name__ == '__main__':
    fb = sys.argv[sys.argv.index('--fb') + 1] if '--fb' in sys.argv else f'{FB_W}x{FB_H}'
    seed_file = sys.argv[sys.argv.index('--seed-file') + 1] if '--seed-file' in sys.argv else None
    w, h = (int(x) for x in fb.split('x'))
    smp = int(sys.argv[sys.argv.index('--smp') + 1]) if '--smp' in sys.argv else 1
    root = build(qemu='--qemu' in sys.argv, fb_w=w, fb_h=h, qemu_cpus=smp,
                 native='--native' in sys.argv, seed_file=seed_file)
    blob = adt.serialize(root)
    open(sys.argv[1], 'wb').write(blob)
    if '--dump' in sys.argv:
        adt.dump(adt.parse(blob)[0])
    print(f'{sys.argv[1]}: {len(blob)} bytes', file=sys.stderr)