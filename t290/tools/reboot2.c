/*
 * reboot2 ARG: sync, then reboot(LINUX_REBOOT_CMD_RESTART2, ARG) directly.
 * Android's "reboot recovery" also leaves "boot-recovery" in the misc
 * partition, so after the shim's own reset LK booted recovery again: a
 * boot loop (2026-10-21). The kernel's restart handler (msm-poweroff) only
 * puts the reason into IMEM (0x77665502 for "recovery"), which LK reads once.
 *
 * Build: aarch64-linux-gnu-gcc -static -O2 reboot2.c -o reboot2
 */
#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/reboot.h>

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: reboot2 recovery|bootloader|...\n");
		return 2;
	}
	sync();
	syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
		LINUX_REBOOT_CMD_RESTART2, argv[1]);
	perror("reboot");
	return 1;
}
