// SPDX-License-Identifier: GPL-2.0
/*
 * a5-diag: hardware helper for the Galaxy A5 (2017) mainline test image.
 * Static aarch64 binary (musl). Called from /init:
 *
 *   a5-diag panel   DECON to hardware (TE) refresh, proven in M1
 *   a5-diag info    kernel, CPUs, memory, eMMC (locked read-only first)
 *   a5-diag usb     USB clock/PHY state as S-Boot left it; turns on the
 *                   USB PLL and CCORE gates if they are off (M4 diagnosis)
 *   a5-diag keys    PID 1 loop: print key presses, hold Power 3 s ->
 *                   reboot into recovery (TWRP), reap orphaned children
 *
 * Register sources: Samsung universal7880 kernel (hybris-17.1)
 *   drivers/soc/samsung/pwrcal/S5E7880/S5E7880-cmusfr.h, -cmu.c, -pmusfr.h
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/reboot.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <linux/fs.h>
#include <linux/input.h>
#include <linux/reboot.h>

#define DECON_BASE		0x14830000UL
#define HW_SW_TRIG_CONTROL	0x70
#define TRIG_HW_CONTINUOUS	0x10

#define CMU_CCORE		0x10680000UL
#define CCORE_BUS_PLL_CON0	0x140
#define CCORE_MEDIA_PLL_CON0	0x120
#define CCORE_MUX_FSYS_BUS	0x284
#define CCORE_MUX_USB20DRD	0x29c
#define CCORE_DIV_FSYS_BUS	0x484
#define CCORE_DIV_USB20DRD	0x49c
#define CCORE_EN_FSYS_BUS	0x884
#define CCORE_EN_USB20DRD	0x89c

#define CMU_FSYS		0x13730000UL
#define FSYS_USB_PLL_LOCK	0x000
#define FSYS_USB_PLL_CON0	0x100
#define FSYS_MUX_USB_PLL	0x200
#define FSYS_MUX_PHYCLK_USER	0x230
#define FSYS_EN_BUS		0x804
#define FSYS_EN_USB20DRD_REF	0x828

#define PMU_BASE		0x106b0000UL
#define PMU_USBDEV_PHY_CONTROL	0x704

#define PLL_ENABLE		(1u << 31)
#define PLL_LOCKED		(1u << 29)
/* Samsung S5E7880-cmu.c usb_pll_rates: 50 MHz = 26 MHz * 400 / (13 << 4) */
#define USB_PLL_50MHZ		((400u << 16) | (13u << 8) | 4u)

static volatile uint32_t *map(unsigned long phys)
{
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	void *p;

	if (fd < 0)
		return NULL;
	p = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, phys);
	close(fd);
	return p == MAP_FAILED ? NULL : p;
}

#define R(b, o)		((b)[(o) / 4])

static void read_file(const char *path, char *buf, size_t len)
{
	FILE *f = fopen(path, "r");
	size_t n = 0;

	buf[0] = 0;
	if (f) {
		n = fread(buf, 1, len - 1, f);
		fclose(f);
	}
	buf[n] = 0;
	while (n && (buf[n - 1] == '\n' || buf[n - 1] == ' '))
		buf[--n] = 0;
}

static double pll141xx_mhz(uint32_t con0)
{
	unsigned int m = (con0 >> 16) & 0x3ff, p = (con0 >> 8) & 0x3f, s = con0 & 7;

	return p ? 26.0 * m / (p << s) : 0;
}

static int cmd_panel(void)
{
	volatile uint32_t *d = map(DECON_BASE);

	if (!d)
		return 1;
	R(d, HW_SW_TRIG_CONTROL) = TRIG_HW_CONTINUOUS;
	return 0;
}

static int cmd_info(void)
{
	char rel[128], cpus[64], mem[4096], name[64], size[32];
	unsigned long long sectors;
	char *p;
	int fd, ro = 1, parts = 0;

	read_file("/proc/sys/kernel/osrelease", rel, sizeof(rel));
	read_file("/sys/devices/system/cpu/online", cpus, sizeof(cpus));
	read_file("/proc/meminfo", mem, sizeof(mem));
	p = strchr(mem, '\n');
	if (p)
		*p = 0;
	printf(" kernel : %s aarch64\n cpus   : %s\n memory : %s\n", rel, cpus, mem);

	for (int i = 0; i < 30 && (fd = open("/dev/mmcblk0", O_RDONLY)) < 0; i++)
		usleep(200000);
	if (fd < 0) {
		printf(" emmc   : NOT FOUND\n");
		return 0;
	}
	ioctl(fd, BLKROSET, &ro);	/* nothing in this image writes the eMMC */
	close(fd);
	read_file("/sys/block/mmcblk0/device/name", name, sizeof(name));
	read_file("/sys/block/mmcblk0/size", size, sizeof(size));
	sectors = strtoull(size, NULL, 10);
	for (int i = 1; i < 64; i++) {
		char path[64];

		snprintf(path, sizeof(path), "/sys/class/block/mmcblk0p%d", i);
		if (access(path, F_OK) == 0)
			parts++;
	}
	printf(" emmc   : %s, %llu MiB, %d partitions, READ-ONLY\n",
	       name, sectors / 2048, parts);
	return 0;
}

static int cmd_usb(void)
{
	volatile uint32_t *cc = map(CMU_CCORE), *fs = map(CMU_FSYS), *pmu = map(PMU_BASE);
	uint32_t bus, media, pll, pll_new;
	double busmhz;

	if (!cc || !fs || !pmu) {
		printf(" usb    : /dev/mem map failed\n");
		return 1;
	}
	bus = R(cc, CCORE_BUS_PLL_CON0);
	media = R(cc, CCORE_MEDIA_PLL_CON0);
	busmhz = pll141xx_mhz(bus);

	for (int i = 0; i < 2; i++) {
		unsigned int mux = R(cc, i ? CCORE_MUX_USB20DRD : CCORE_MUX_FSYS_BUS);
		unsigned int div = R(cc, i ? CCORE_DIV_USB20DRD : CCORE_DIV_FSYS_BUS);
		unsigned int en = R(cc, i ? CCORE_EN_USB20DRD : CCORE_EN_FSYS_BUS);
		double parent = ((mux >> 12) & 1) ? pll141xx_mhz(media) / 2 : busmhz / 2;

		printf(" ccore  : %-8s mux %08x div %08x en %08x = %.3f MHz%s\n",
		       i ? "USB20DRD" : "FSYS_BUS", mux, div, en,
		       parent / ((div & 0xf) + 1), (en & 1) ? "" : "  GATED");
		if (!(en & 1)) {
			R(cc, i ? CCORE_EN_USB20DRD : CCORE_EN_FSYS_BUS) = en | 1;
			printf(" ccore  : %s gate turned ON\n", i ? "USB20DRD" : "FSYS_BUS");
		}
	}

	pll = R(fs, FSYS_USB_PLL_CON0);
	printf(" fsys   : USB_PLL %08x = %.3f MHz %s%s\n", pll, pll141xx_mhz(pll),
	       (pll & PLL_ENABLE) ? "on" : "OFF", (pll & PLL_LOCKED) ? " locked" : "");
	if (!(pll & PLL_ENABLE)) {
		pll_new = (pll & ~0x03ff3f07u) | USB_PLL_50MHZ;
		R(fs, FSYS_USB_PLL_LOCK) = 13 * 200;	/* >= 200 x PDIV cycles */
		R(fs, FSYS_USB_PLL_CON0) = pll_new;
		R(fs, FSYS_USB_PLL_CON0) = pll_new | PLL_ENABLE;
		for (int i = 0; i < 1000 && !(R(fs, FSYS_USB_PLL_CON0) & PLL_LOCKED); i++)
			usleep(10);
		pll = R(fs, FSYS_USB_PLL_CON0);
		printf(" fsys   : USB_PLL set to %08x = %.3f MHz %s\n", pll,
		       pll141xx_mhz(pll), (pll & PLL_LOCKED) ? "locked" : "NOT LOCKED");
	}
	printf(" fsys   : mux_usb_pll %08x phyclk_user %08x en_bus %08x en_ref %08x\n",
	       R(fs, FSYS_MUX_USB_PLL), R(fs, FSYS_MUX_PHYCLK_USER),
	       R(fs, FSYS_EN_BUS), R(fs, FSYS_EN_USB20DRD_REF));
	printf(" pmu    : USBDEV_PHY_CONTROL %08x\n", R(pmu, PMU_USBDEV_PHY_CONTROL));
	return 0;
}

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

static const char *key_name(int code)
{
	switch (code) {
	case KEY_POWER:		return "POWER";
	case KEY_VOLUMEUP:	return "VOLUME UP";
	case KEY_VOLUMEDOWN:	return "VOLUME DOWN";
	case KEY_HOMEPAGE:	return "HOME";
	default:		return "key";
	}
}

static int cmd_keys(void)
{
	struct pollfd fds[8];
	int nfds = 0;
	long long power_down = -1;

	for (int i = 0; i < 8; i++) {
		char path[32];
		int fd;

		snprintf(path, sizeof(path), "/dev/input/event%d", i);
		fd = open(path, O_RDONLY | O_NONBLOCK);
		if (fd >= 0) {
			fds[nfds].fd = fd;
			fds[nfds++].events = POLLIN;
		}
	}
	for (;;) {
		int r = poll(fds, nfds, 200);

		while (waitpid(-1, NULL, WNOHANG) > 0)
			;	/* PID 1 reaps orphans */
		if (power_down >= 0 && now_ms() - power_down >= 3000) {
			printf("\n POWER held 3 s -> rebooting into recovery (TWRP)...\n");
			fflush(stdout);
			sync();
			sleep(1);
			syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
				LINUX_REBOOT_CMD_RESTART2, "recovery");
		}
		if (r <= 0)
			continue;
		for (int i = 0; i < nfds; i++) {
			struct input_event ev;

			if (!(fds[i].revents & POLLIN))
				continue;
			while (read(fds[i].fd, &ev, sizeof(ev)) == sizeof(ev)) {
				if (ev.type != EV_KEY || ev.value == 2)
					continue;
				printf(" %-11s %s\n", key_name(ev.code),
				       ev.value ? "pressed" : "released");
				fflush(stdout);
				if (ev.code == KEY_POWER)
					power_down = ev.value ? now_ms() : -1;
			}
		}
	}
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc < 2)
		goto usage;
	if (!strcmp(argv[1], "panel"))
		return cmd_panel();
	if (!strcmp(argv[1], "info"))
		return cmd_info();
	if (!strcmp(argv[1], "usb"))
		return cmd_usb();
	if (!strcmp(argv[1], "keys"))
		return cmd_keys();
usage:
	fprintf(stderr, "usage: a5-diag panel|info|usb|keys\n");
	return 2;
}
