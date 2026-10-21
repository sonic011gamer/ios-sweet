/* Linux sysinfo() for the iPadOS build of Mesa Freedreno (t290/mesa): RAM size only */
#ifndef T290_SYS_SYSINFO_STUB_H
#define T290_SYS_SYSINFO_STUB_H
#include <string.h>
#include <sys/sysctl.h>
struct sysinfo {
	unsigned long totalram, freeram;
	unsigned int mem_unit;
};
static inline int sysinfo(struct sysinfo *si)
{
	uint64_t mem = 0;
	size_t len = sizeof(mem);
	memset(si, 0, sizeof(*si));
	si->mem_unit = 1;
	if (sysctlbyname("hw.memsize", &mem, &len, NULL, 0) == 0)
		si->totalram = (unsigned long)mem;
	return 0;
}
#endif
