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

## Ядро T290 (2026-10-19) — написано, ЕЩЁ НЕ СОБИРАЛОСЬ

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
