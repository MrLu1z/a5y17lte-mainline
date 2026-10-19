// SPDX-License-Identifier: GPL-2.0
/*
 * a5-diag: hardware helper for the Galaxy A5 (2017) mainline test image.
 * Static aarch64 binary (musl). Called from /init:
 *
 *   a5-diag panel   DECON to hardware (TE) refresh, proven in M1
 *   a5-diag info    kernel, CPUs, memory, eMMC (locked read-only first)
 *   a5-diag usb     USB clock/PHY state as S-Boot left it; turns on the
 *                   USB PLL and CCORE gates if they are off (M4 diagnosis)
 *   a5-diag muic    read the S2MU004 MUIC over /dev/i2c-N and, if the USB
 *                   D+/D- switch is not on USB, set manual mode + USB
 *   a5-diag usbregs DWC3 and USB PHY register dump, UDC state
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
#include <dirent.h>
#include <linux/fs.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
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

/* ---- S2MU004 MUIC (Samsung include/linux/mfd/samsung/s2mu004-private.h,
 *      include/linux/muic/s2mu004-muic.h, drivers/muic/s2mu004-muic.c) ---- */
#define MUIC_ADDR		0x3d
#define MUIC_ADC		0x61
#define MUIC_DEVICE_TYPE1	0x62
#define MUIC_DEVICE_TYPE2	0x63
#define MUIC_DEVICE_TYPE3	0x64
#define MUIC_CHG_TYPE		0x68
#define MUIC_REV_ID		0x82
#define MUIC_CTRL1		0xc7
#define MUIC_SW_CTRL		0xca
#define CTRL1_MANUAL_SW		(1 << 2)	/* 1 = auto switching, 0 = manual */
#define SW_DM_DP_MASK		0xfc
#define SW_USB			((1 << 5) | (1 << 2))	/* DM=USB, DP=USB */

static int i2c_fd = -1;

static int i2c_open_muic(void)
{
	DIR *d = opendir("/sys/bus/i2c/devices");
	struct dirent *e;
	char path[96], name[64];

	if (!d)
		return -1;
	while ((e = readdir(d))) {
		if (strncmp(e->d_name, "i2c-", 4))
			continue;
		snprintf(path, sizeof(path), "/sys/bus/i2c/devices/%s/name", e->d_name);
		read_file(path, name, sizeof(name));
		if (strstr(name, "muic")) {
			snprintf(path, sizeof(path), "/dev/%s", e->d_name);
			i2c_fd = open(path, O_RDWR);
			printf(" muic   : bus %s (%s)\n", e->d_name, name);
			break;
		}
	}
	closedir(d);
	return i2c_fd;
}

static int muic_rd(uint8_t reg)
{
	uint8_t val = 0;
	struct i2c_msg msgs[2] = {
		{ .addr = MUIC_ADDR, .flags = 0, .len = 1, .buf = &reg },
		{ .addr = MUIC_ADDR, .flags = I2C_M_RD, .len = 1, .buf = &val },
	};
	struct i2c_rdwr_ioctl_data x = { .msgs = msgs, .nmsgs = 2 };

	return ioctl(i2c_fd, I2C_RDWR, &x) == 2 ? val : -1;
}

static int muic_wr(uint8_t reg, uint8_t val)
{
	uint8_t buf[2] = { reg, val };
	struct i2c_msg msg = { .addr = MUIC_ADDR, .flags = 0, .len = 2, .buf = buf };
	struct i2c_rdwr_ioctl_data x = { .msgs = &msg, .nmsgs = 1 };

	return ioctl(i2c_fd, I2C_RDWR, &x) == 1 ? 0 : -1;
}

static const char *sw_path(int v)
{
	switch ((v >> 2) & 7) {	/* DP field; DM mirrors it for normal paths */
	case 0: return "OPEN";
	case 1: return "USB";
	case 2: return "UART";
	case 3: return "UART2";
	default: return "?";
	}
}

static int cmd_muic(void)
{
	int ctrl, sw, rev;

	if (i2c_open_muic() < 0) {
		printf(" muic   : no i2c-gpio-muic bus\n");
		return 1;
	}
	rev = muic_rd(MUIC_REV_ID);
	ctrl = muic_rd(MUIC_CTRL1);
	sw = muic_rd(MUIC_SW_CTRL);
	if (rev < 0 || ctrl < 0 || sw < 0) {
		printf(" muic   : I2C read FAILED (rev %d ctrl %d sw %d)\n", rev, ctrl, sw);
		return 1;
	}
	printf(" muic   : rev %02x adc %02x dev %02x %02x %02x chg %02x\n", rev,
	       muic_rd(MUIC_ADC), muic_rd(MUIC_DEVICE_TYPE1),
	       muic_rd(MUIC_DEVICE_TYPE2), muic_rd(MUIC_DEVICE_TYPE3),
	       muic_rd(MUIC_CHG_TYPE));
	printf(" muic   : CTRL1 %02x (%s switching)  SW_CTRL %02x (D+/D- = %s)\n",
	       ctrl, (ctrl & CTRL1_MANUAL_SW) ? "auto" : "manual", sw, sw_path(sw));

	if ((sw & SW_DM_DP_MASK) != SW_USB || (ctrl & CTRL1_MANUAL_SW)) {
		muic_wr(MUIC_SW_CTRL, (sw & 0x03) | SW_USB);
		muic_wr(MUIC_CTRL1, ctrl & ~CTRL1_MANUAL_SW);
		ctrl = muic_rd(MUIC_CTRL1);
		sw = muic_rd(MUIC_SW_CTRL);
		printf(" muic   : SET  CTRL1 %02x (%s)  SW_CTRL %02x (D+/D- = %s)\n",
		       ctrl, (ctrl & CTRL1_MANUAL_SW) ? "auto" : "manual", sw, sw_path(sw));
		return 10;	/* tell /init to re-attach the gadget */
	}
	return 0;
}

static int cmd_usbregs(void)
{
	volatile uint32_t *dwc = map(0x1360c000), *phy = map(0x135c0000);
	char st[64] = "";
	DIR *d = opendir("/sys/class/udc");
	struct dirent *e;

	while (d && (e = readdir(d))) {
		char path[96];

		if (e->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "/sys/class/udc/%s/state", e->d_name);
		read_file(path, st, sizeof(st));
	}
	if (d)
		closedir(d);
	printf(" udc    : state %s\n", st[0] ? st : "?");
	if (dwc)	/* DWC3 global/device registers at 0xc000 */
		printf(" dwc3   : GCTL %08x GSTS %08x U2PHY %08x\n"
		       "          DCFG %08x DCTL %08x DSTS %08x\n",
		       R(dwc, 0x110), R(dwc, 0x118), R(dwc, 0x200),
		       R(dwc, 0x700), R(dwc, 0x704), R(dwc, 0x70c));
	if (phy)	/* mainline phy-exynos5-usbdrd.c EXYNOS5/7870 offsets */
		printf(" phy    : LINKSYS %08x UTMI %08x CLKRST %08x\n"
		       "          HSPHYCTRL %08x PLLTUNE %08x\n",
		       R(phy, 0x04), R(phy, 0x08), R(phy, 0x10),
		       R(phy, 0x54), R(phy, 0x70));
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
	if (!strcmp(argv[1], "muic"))
		return cmd_muic();
	if (!strcmp(argv[1], "usbregs"))
		return cmd_usbregs();
	if (!strcmp(argv[1], "keys"))
		return cmd_keys();
usage:
	fprintf(stderr, "usage: a5-diag panel|info|usb|muic|usbregs|keys\n");
	return 2;
}
