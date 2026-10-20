# ios-t290 — iPadOS 16 на Samsung Galaxy Tab A 8.0 (2019, SM-T290, `gtowifi`)

Порт по образцу [dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet)
(XNU + неизменённый userland iOS 16 на Redmi Note 10 Pro). Локальная копия:
`D:\test1\ios-sweet`, его `NOTES.md` — подробный журнал того, как это делалось.

## Статус (2026-10-19)

Этап 0 — разведка. Планшет подключён по adb (только чтение), на нём ничего не меняли.

### Проверено на живом планшете (adb, без root, лог `logs/probe-20261019-225532.txt`, не в git)

- SM-T290, `gtowifi` / product `gtowifiser`, Android 11, прошивка **T290XXS5CWG5**
  (загрузчик ревизии 5 — откат на прошивку с меньшей ревизией невозможен), патч 2023-05-01.
- `ro.board.platform msm8937`, Hardware `Qualcomm Technologies, Inc SDM429`.
- 4 ядра, `CPU part 0xd03` = **Cortex-A53** r0p4; Features `fp asimd evtstrm aes pmull sha1 sha2 crc32` —
  **нет `atomics` (LSE)**, то есть чистый ARMv8.0. Подтверждает выбор userland iPad A9.
- RAM: MemTotal 1 892 156 kB (~1.8 ГБ доступно ядру).
- Экран 800×1280, density 213.
- Тач: `fts_ts` (FocalTech) на **i2c-3 @ 0x38** — совпадает с DTS P81081. Значит, файлы
  `sdm429-mtp-P81081*` действительно от T290.
- Остальное на I²C тоже совпадает с P81081: 5-0020 ABOV SAR («ABOV Cap Touch bottom»),
  8-0058/8-0059 усилители Awinic; ещё 2-0008/000c/000f/0045 (зарядка/fuel gauge/датчики — уточнить).
- Кнопки: `qpnp_pon` (питание и, видимо, громкость−) и `soc:gpio_keys` (громкость+).
- Загрузчик: **заблокирован** (`ro.boot.flash.locked 1`, `verifiedbootstate green`).
  `sys.oem_unlock_allowed 1` — переключатель «Заводская разблокировка» уже включён.
- eMMC `mmcblk0`, раздел **aboot** (= LK) есть. Ключевые разделы: boot p58, recovery p57,
  vbmeta p17, dtbo p27, userdata p69, persist p48, efs p43, sec_efs p42, modemst1/2 p40/p41,
  fsg p36, fsc p30, param p47. Размеры разделов, cmdline и /proc/device-tree без root не читаются.

## 2026-10-20 ~17:00: планшет разблокирован, TWRP, живой FDT

- Загрузчик разблокирован пользователем (`ro.boot.flash.locked=0`, `verifiedbootstate=orange`),
  прошивка та же T290XXS5CWG5 / Android 11. Knox сгорел (необратимо).
- TWRP 3.7.0_11-HeribertYavuz (XDA «OFRP & TWRP For TAB A 2019 (T290) (Binary 5)») прошит Odin 3.14
  в AP одним tar: `recovery.img` автора + свой пустой `vbmeta.img` (AVB0, flags=2, 4096 Б) —
  `D:\test1\dl\twrp\twrp-AP.tar`. Стоковый vbmeta перезаписан → для отката нужна стоковая CWG5.
  Вход в TWRP: Odin без Auto Reboot → Vol−+Power до чёрного → сразу Vol+ +Power.
  Android грузится как раньше (boot/system/userdata не трогали). В TWRP adb = root.
- Дампы (вне git): `D:\test1\ios-t290\logs\twrp-20261020\` (fdt.dtb/fdt.dts, iomem, dmesg, partitions,
  getprop, interrupts); бэкап всех разделов ≤200 МБ (64 шт., 712 МБ, sha256): `D:\test1\ios-t290\backup-20261020\`
  (RPMB не читается — норма). Большие (system/vendor/product/oem/cache/userdata) не копировались.

- **Root + постоянный TWRP (17:20).** Android при каждой загрузке восстанавливал стоковый recovery
  (`/system/bin/install-recovery.sh` + `/system/recovery-from-boot.p`, applypatch только если boot = сток,
  sha1 10b1f840…). Решение: Magisk v30.7 (GitHub, sha256 e0d32d21…) патчит стоковый boot на ПК
  (`magiskboot` x86_64 из APK, `boot_patch.sh`, `LEGACYSAR=true KEEPFORCEENCRYPT=true KEEPVERITY=false`;
  в стоковом boot нет ramdisk, root=PARTUUID — legacy SAR) → `fix_bootable_image.py`
  (GalaxyA20s/A20s-Fix-Bootable-Image: `SignerVer02` + AVBf-размер, иначе SECURE CHECK FAIL) →
  Odin AP `D:\test1\dl\root\magisk-twrp-AP.tar` (boot+TWRP+пустой vbmeta). Откат boot: `stock-boot-AP.tar`.
  Проверено: Android грузится, `magiskd` работает, `adb shell /debug_ramdisk/su -c id` = root;
  после загрузки Android sha256 разделов recovery/boot = прошитым образам → TWRP больше не слетает.
  **Значит, наше ядро iPadOS в `recovery` Android тоже не затрёт.**

Живой FDT (board_id P81080BA5, `qcom,sdm429`, msm-id 0x162; панель `jd9367_wxga_video_txd`):
| Что | Значение |
| --- | --- |
| RAM | `0x80000000`+1 ГБ и `0xc0000000`+1 ГБ = сплошные 2 ГБ `0x80000000..0x100000000` |
| Свободно для XNU | крупнейший кусок System RAM: **`0x92600000..0xffffffff` (~1754 МБ)**; ниже — дыры reserved |
| reserved no-map | 0x85000000+0x800000, 0x85b00000+0xd00000, 0x86800000+0x5500000, 0x8bd00000+0x1100000, 0x8ce00000+0x700000; splash 0x90000000+0x1400000; ramoops 0x92100000+0x100000; 0x92200000+0x400000 |
| Splash fb | 0x90000000, 800×1280, 32 bpp, stride 3200 (`mdssfb_80000`) |
| GIC | v2 `qcom,msm-qgic2`, GICD 0x0b000000, GICC 0x0b002000 (подтверждено) |
| Таймер | 19.2 МГц, PPI <1 2>,<1 3>,<1 4>,<1 1> → **vtimer INTID 20** (подтверждено) |
| CPU | 4× cortex-a53, MPIDR 0x100..0x103, `enable-method = psci`, **PSCI 1.0 method = smc** (в QEMU — hvc) |
| eMMC | sdhc1 `qcom,sdhci-msm` hc 0x7824900, core 0x7824000, cmdq 0x7824e00; IRQ SPI 123 (hc), 138 (pwr); 8 бит; 32 ГБ (QX63AB) |
| **microSD** | **sdhc2** hc 0x7864900, core 0x7864000; IRQ SPI 125 (hc, INTID 157), 221 (pwr); 4 бит; **card-detect tlmm GPIO 67**; питание vdd/vdd-io — регуляторы PMIC (могут быть выключены LK — проверить) |
| UART | msm-uartdm 0x78b0000 (earlycon), IRQ SPI 108 |

План пользователя: iPadOS-корень на **microSD** (sdhc2), ядро — в раздел `recovery` (вход Vol+ +Power),
Android в `boot` не трогать. Нужно: драйвер SDHCI (проверить в QEMU), карта памяти в mkadt_t290
(база 0x92600000?), PSCI smc, формат Samsung boot.img v1 (ядро + дописанный DTB) для LK.

## 🎉🎉 2026-10-20 ~16:40: ЭКРАН БЛОКИРОВКИ iPadOS 16 в QEMU (прогон ios3)

Тот же прогон ios3 (CI 39321740004, SMP=1, TCG): логотип Apple ~240–1620 с, затем ~1620–1860 с
на экране консоль ядра (`SK[0]: __nexus_register identityservices(53)...`), первый `SWEET FB: swap`
~1840 с, к **1920 с — lock screen SpringBoard**: «Wednesday, December 31», часы, «Press home to open»,
батарея 100% (снимки `D:\test1\qemu-out\ios3\shot-1920.png` … `shot-2160.png`).
Вывод: ядро T290 (A53, 4K, GICv2, virtio-blk) + неизменённый userland iPad 5 доходят до GUI;
«зависание» на логотипе было просто медленной первой загрузкой (datamigrator) на 1 vCPU.
Следующее: SMP=4 (быстрее?), ввод в QEMU (Home/тач) → домашний экран; потом путь на планшет
(LK/Odin, eMMC SDHCI, экран 800×1280, тач FocalTech).

## 2026-10-20 ~16:00: iPadOS userland в QEMU, логотип Apple (прогон ios3)

CI 39321740004 (189dc00), `qemu-ios.sh` SMP=1 на свежей копии корня iPad 5:
корень `vblkroot0` смонтирован, **launchd стартовал**, через ~800 с — около 100 демонов
(`exec ...` в msgbuf), SpringBoard (pid 25) и backboardd (pid 56) живы, на экране **логотип Apple**.
Ошибок virtio-blk нет. Стеки SpringBoard/backboardd на 242 с и 365 с совпадают — ждут в mach_msg
(наблюдение); при этом работает `com.apple.datamigrator` — гипотеза: первая загрузка, SpringBoard
ждёт миграцию данных, а TCG на одном vCPU очень медленный. Не проверено.
Результаты: `D:\test1\qemu-out\ios3\shot-*.png`, `D:\test1\qemu-out\mb-ios3.txt` (вне git).

Почему ios2 падал (`SWEET VBLK: ... status 255`, `load_init_program ... errno 5`): кончилось место на C:
(ext4.vhdx WSL вырос до 105 ГБ). Теперь большие файлы — на D:: чистый корень
`D:\test1\ios-images\ipad5-root.pristine.img`, рабочая копия `/root/ios/TAG.img` в WSL (удалять после прогона),
старое содержимое WSL — `D:\wsl-backup\` (tar). QEMU нужно держать в foreground-вызове `wsl`:
`nohup ... &` внутри `wsl bash -c` умирает при выходе wsl.exe.

## 🎉 2026-10-20 ~03:40: ядро T290 доходит до userspace в QEMU

CI 39309753629 (0101–0110 + firehose -mcpu в workflow), `qemu-t290.sh` (cortex-a53, GICv2,
RAM-диск init): `BSD root: md0` → `*** SWEET userspace: init running as PID 1 ***`.
Цепочка исправлений этого дня (каждое найдено в QEMU):
- 0104 ранняя консоль телефона (0x9c000000, 16K-таблицы) → выключена для T290;
- 0105 `casa` в commpage (pfz trylock) → ldaxr/stxr;
- `-mcpu=cortex-a53` (clang для arm64-apple-macos держит apple-m1 и LSE при любом -march),
  то же для libfirehose_kernel (workflow, xcconfig `OTHER_CFLAGS[arch=arm64]`);
- 0106/0108 kmem claims — из самой большой дыры kernel_map + запас 1/32, лог каждого claim;
- 0107 zone map iOS (24 ГБ VA вместо 128), 0109 пул компрессора iOS (RAM вместо 16×RAM);
- ADT: CPU compatible должен содержать `qcom,kryo470` (IONameMatch SweetCPU), иначе вечное ожидание CPU;
- 0110 commpage text VA выровнен на PAGE_RATIO (4K железо + 16K VM-страницы → write_pte_fast panic).
Загрузка ждёт ~30 с RTC (в QEMU его нет) — не блокер.

Отладка: `t290/tools/kmem.py PORT KERNEL msgbuf|bt|regs|read` через gdb-stub QEMU
(`qemu-sweet.py --gdbport`); `D:\test1\dl\ci-boot.sh RUN TAG` = дождаться CI → скачать →
скан LSE во всём ядре (`dl/bt/kscan.sh`) → загрузка с msgbuf/bt (`dl/bt/dbgboot.sh`).

Образ iPadOS: чистый — `D:\test1\ios-images\ipad5-root.pristine.img` (раньше `/root/ios/` в WSL), 11 ГБ на HFS+ 16 ГБ, fsck OK.
Собран `t290/tools/mkroot-ipad5.sh` (stage в ext4 → tar → KVM-гость с Ubuntu-ядром,
hfsplus.ko + nls_utf8.ko + busybox распаковывает в HFS+). Предупреждение: в образе iPad нет
system-контейнера mobileactivationd (data ark не записан).

## Состояние 2026-10-20 (утро)

- Форк: github.com/terx1232/ios-sweet, ветка `t290` (ветка по умолчанию форка — иначе GitHub
  не регистрировал workflow). Сборка: `gh workflow run xnu.yml -R terx1232/ios-sweet --ref t290 -f sweet_board=t290`.
- CI 39280792571 (0101+0102 старой версии): упал только на pe_fiq.c и IOKitBSDInit.cpp —
  `\n` в трёх строках kprintf/IOLog превратился в перевод строки (heredoc через Bash-инструмент
  схлопывает `\\` → `\`; писать такие правки через Write/Edit). Флаги T290 в CI дошли до 1101
  файла, патчи 0101/0102 применились. Исправлено в eee22c1, CI 39285650537 запущен.
- Патчи: 0101 (A53/4K/GICv2), 0102 (virtio-blk root, `rd=vblkroot0 vblkrw=1`),
  0103 (PSCI через HVC по `sweetpsci=hvc`, для QEMU SMP).
- **Локальная проверка компиляции работает** (WSL, clang 14 из Ubuntu):
  дерево `xnu-buildtree` + лог CI автора (run 39158169212) → `~/bt`, на него наложены 0101–0103;
  `XNU_CC_EXTRA=-enable-trivial-auto-var-init-zero-knowing-it-will-be-removed-from-clang`,
  `XNU_CC_POST="-DSWEET_T290=1 -march=armv8-a+crc+crypto -UARM_LARGE_MEMORY -UVM_KERNEL_LINK_ADDRESS -DVM_KERNEL_LINK_ADDRESS=0xfffffff007004000"`,
  `python3 tools/xnu-cc.py ~/bt/run.log ~/bt --per-file` (новые файлы вроде sweet_vblk.c — только явным путём).
  Результат: всё ядро проходит, падают лишь хостовые `SETUP/*` (как и у sweet). Ошибки CI совпали с локальными 1:1.
- iOS: iPad 5 (iPad6,11/J71sAP) iPadOS 16.7.16 20H392, IPSW качается в `D:\test1\ipsw\` (вне git).
  OS = 087-86634-044.dmg, SystemOS cryptex = 087-86624-047.dmg, AppOS = 087-86715-047.dmg.
  `research/ipsw-ipad5/j71sap.adt` (DeviceTree, вне git) → /product для mkadt_t290.
- WSL без сети: служба Windows **HNS отключена** (StartupType Disabled), нужен админ
  (`Set-Service hns -StartupType Manual; Start-Service hns`). Обход: .deb скачаны через Windows
  (`D:\test1\dl\debs`: hfsprogs, libfuse3-dev, libbz2-dev), apfs-fuse собран g++ без cmake
  (`/usr/local/bin/apfs-fuse`, скрипт `D:\test1\dl\build-apfs.sh`).
- QEMU: `t290/tools/qemu-ios.sh KERNEL ROOT.img OUT` — 4×A53, GICv2, virtio-blk корень,
  экран 1536×2048 (AppleCLCD сам выбирает родной размер, если буфер больше 1125×2436),
  снимки каждые 30 с (`--shots`) = замер времени загрузки.

## Ядро T290 (2026-10-19) — первая версия

Порт живёт веткой `t290` форка ios-sweet. Отдельной MACHINE_CONFIG нет: весь код
sweet (80+ мест `ARM64_BOARD_CONFIG_SWEET`, стейдж-скрипты по `MACHINE_CONFIG == SWEET`)
используется как есть, плата выбирается переменной окружения **`SWEET_BOARD=t290`**
(CI: input `sweet_board: t290`). Без неё патч 0101 ничего не меняет — сборка sweet прежняя.

`xnu/patches/0101-t290-a53-4k-gicv2.patch` (сделан на xnu-8796.141.3 + 0001–0081, все 81 патч sweet
легли локально; патчи darwin-xnu-build не пересекаются с файлами 0101):
- `MakeInc.def`: `-DSWEET_T290=1 -march=armv8-a+crc+crypto`; iOS-раскладка ядра
  (`-UARM_LARGE_MEMORY`, link 0xfffffff007004000) — macOS-раскладка на −2 ТБ не помещается
  в 38-битное пространство 4K-режима (T1SZ 26).
- `SWEET.h`: при `SWEET_T290` нет `__ARM_16K_PG__` и `__ARM_PAN_AVAILABLE__`,
  есть `__ARM64_PMAP_SUBPAGE_L1__` (как BCM2837), `HAS_GIC_V2`, 4 CPU, vtimer INTID 20,
  регистры GICv2.
- `vm_param.h`: `MACH_VM_MAX_ADDRESS` = iOS 0xFC0000000 (макОС-значение 47 бит не влезает в T0SZ 26).
- `pe_fiq.c`: драйвер GICv2 (GICD+GICC по `/arm-io/gic reg`, маска интерфейса ЦП из
  GICD_ITARGETSR0, `sweet_gic_ack/eoi/send_sgi`); `sleh.c`: IAR/EOIR через GICC;
  `SweetCPU.cpp`: IPI через GICD_SGIR; `start.s`: ICC_SRE_EL2 только при GICv3.

Проверено на практике: готовое ядро sweet (CI 18.10) на QEMU `-cpu cortex-a53` падает на
первой же LSE-инструкции `ldadd` (0xb8280028, `lck_grp_reference`) — флаг `-march=armv8-a`
обязателен. QEMU 6.2 при этом не отверг 16K-таблицы (MMU включился) — гранулу он не
проверяет; для строгой проверки нужен QEMU ≥ 7.2.

Инструменты: `t290/tools/mkadt_t290.py [--qemu]` (дерево sweet + GICv2, 4×A53 MPIDR 0x10x,
RAM 0xa0000000+1.5 ГБ, fb 0x90000000 800×1280, без sweet-ufs), `tools/qemu-sweet.py`
получил `--gic-version` и `--fb`, `t290/tools/qemu-t290.sh KERNEL OUT` — запуск в QEMU.

Следующий шаг: форк на GitHub пользователя → `gh workflow run xnu.yml -f sweet_board=t290`
→ исправлять ошибки компиляции → `qemu-t290.sh`. Ожидаемый успех: `SWEET GIC: GICv2 ...`,
`init running as PID 1`, как у sweet.

Не проверено / известные вопросы:
- 4K-путь pmap в xnu-8796 никто не собирал для этой конфигурации — возможны ошибки компиляции.
- Драйверы sweet (кнопки SPMI, тач GENI, USB, RPMh) обращаются к MMIO SM7150 — на планшете их
  надо выключить boot-args (`sweetkeys=0 sweettouch=0`, sweetusb по умолчанию выкл.) или загейтить по плате.
- PSCI на SDM429 (smc/hvc) не проверен; в QEMU virt PSCI идёт через hvc — пока cpus=1.
- Формат splash-буфера (32 bpp?) и точная карта памяти — сверить с живым FDT.

## Решения

- **Личность устройства: iPad (5th gen), iPad6,11 / J71sAP, iPadOS 16.7.x.**
  A9 = ARMv8.0 (как Cortex-A53), 2 ГБ RAM (как T290), планшет, официально
  поддерживает iPadOS 16. Userland iPhone X (A11, ARMv8.2) мог бы содержать
  инструкции, которых нет на A53 (LSE-атомики и т.п.) — с A9 этот риск снят.
- **4K-страницы вместо 16K.** Cortex-A53 не поддерживает гранулу 16 КБ
  (ARM DDI 0500F, ID_AA64MMFR0_EL1.TGran16 = 0). XNU 8796 сохранил 4K-путь:
  `osfmk/arm64/proc_reg.h` `#ifdef __ARM_16K_PG__ ... #else ARM_PGSHIFT 12`,
  `osfmk/arm/pmap/pmap.c` `native_pt_attr = &pmap_pt_attr_4k` без `__ARM_16K_PG__`.
  Образец — официальная конфигурация Apple **BCM2837** (Raspberry Pi 3, тоже A53):
  `pexpert/pexpert/arm64/BCM2837.h` — без `__ARM_16K_PG__`, с
  `__ARM64_PMAP_SUBPAGE_L1__`, MAX_CPUS 4. sweet (patch 0004) наоборот
  задаёт `__ARM_16K_PG__ 1` — его конфиг платы НЕ переносить как есть.
- Ядро собирается так же, как в sweet: GitHub Actions macOS + blacktop/darwin-xnu-build
  (нужен форк и аккаунт GitHub пользователя).

## Железо (из LineageOS android_kernel_samsung_sdm429, ветка lineage-22.2, 56a0722)

ВНИМАНИЕ: файлы платы `sdm429-mtp-P81081*.dts*` (board-id <8 0>, Wingtech,
комментарии «N8-R Project») — предположительно T290, **сверить с живым FDT
планшета** (`/sys/firmware/fdt`, нужен root/TWRP).

| Что | Значение | Отличие от sweet |
| --- | --- | --- |
| SoC | SDM429 (семейство MSM8937), 4× Cortex-A53, MPIDR 0x100..0x103 (Aff1=1) | A55/A76 |
| Прерывания | **GICv2** `qcom,msm-qgic2`: GICD 0x0b000000, GICC 0x0b002000 | GICv3 → нужен драйвер GICv2 |
| Таймер | 19.2 МГц, PPI по-квалкомовски `<1 2>,<1 3>,<1 4>,<1 1>` → виртуальный таймер = PPI 4 = INTID 20 (проверить) | у sweet INTID 19 |
| SMP | PSCI (`enable-method = "psci"`) | так же |
| Накопитель | **eMMC**, `sdhc_1` = `qcom,sdhci-msm` @ 0x7824900 (hc), 0x7824000 (core); стандартный SDHCI | UFS → новый драйвер, но SDHCI проще |
| Экран | 800×1280 DSI, continuous splash: `cont_splash_mem` 0x90000000, 0x1400000 | 1080×2400 @ 0x9c000000 |
| Тач | FocalTech (ft3528/ft8201…) I²C 0x38 на `i2c_3`, IRQ tlmm 65, reset tlmm 64, питание pm8953 L10 / L5 | Goodix GT9896 |
| PMIC | PM8953 + PMI632 | PM6150/PM6150L |
| Загрузчик | LK/aboot эпохи MSM8937, `CONFIG_BUILD_ARM64_APPENDED_DTB_IMAGE=y` (DTB дописывается к Image) | ABL, boot.img v2 с отдельным DTB |
| Прошивка | Odin / Download mode, нет `fastboot boot` | fastboot |

Reserved-memory (no-map, низкий банк): 0x85b00000+0xd00000, 0x86800000+0x5000000,
0x8b800000+0x1100000, 0x8c900000+0x700000, ramoops 0x92100000, ss_plog 0x92200000,
removed-dma-pool 0x85000000+0x800000.

## Риски (по убыванию)

1. 4K-путь XNU для iOS 16 никто давно не собирал — возможны поломки компиляции/рантайма.
   Userland iPadOS для A9 исполнялся на 16K-ядре: проверить, не зашиты ли 16K-предположения
   (dyld shared cache, libmalloc, vm_page_size).
2. Samsung: разблокировка сжигает Knox (необратимо), стирает данные; vbmeta/AVB;
   каждый тест = прошивка boot через Odin (нет временной загрузки).
3. Гипервизор Qualcomm/Samsung на EL2 может ограничивать доступ к памяти/MMIO.
4. 2 ГБ RAM и 4×A53 — рисование интерфейса CPU будет медленным.

## План

0. **Разведка (сейчас).** Подключить планшет по adb (`t290/tools/t290-probe.sh`): модель,
   прошивка, статус OEM unlock, ядро. Потом TWRP → бэкап разделов + живой FDT.
1. Shim: свой код стартует из LK и рисует текст в splash-буфере 0x90000000.
2. XNU (4K, конфиг SWEET→T290 по образцу BCM2837, GICv2) — сначала в QEMU (`virt`,
   `-cpu cortex-a53`, gic-version=2), потом на планшете, лог в splash/eMMC.
3. Драйвер eMMC (SDHCI), корень HFS+ из IPSW iPad6,11.
4. Экран (AppleCLCD 800×1280 → iPad 1536×2048?), тач FocalTech, кнопки PM8953 → SpringBoard.
