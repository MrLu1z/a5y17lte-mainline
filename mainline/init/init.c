// SPDX-License-Identifier: GPL-2.0
/*
 * Test init for the Galaxy A5 (2017) on mainline Linux.
 * Built with the kernel's nolibc (tools/include/nolibc), static, no libc.
 *
 * Milestone 1 (confirmed on hardware, EVIDENCE.md section 15):
 *  - DECON in hardware (TE) trigger mode refreshes the command-mode panel
 *    by itself (HW_SW_TRIG_CONTROL = 0x10).
 *  - reboot(RESTART2, "recovery") lands in TWRP.
 *
 * Milestone 2 (confirmed on hardware, EVIDENCE.md section 16a):
 *  - Power, Volume Up/Down produce key events; Power held 3 s -> TWRP.
 *
 * Milestone 3 result (EVIDENCE.md section 17a): card identified, but data
 * reads fail at 50 MHz while EXT_CSD read at ~300 kHz worked.
 *
 * Milestone 3b result (EVIDENCE.md 17b): MMC0 CIU is 399.75 MHz; data reads
 * fail at every clock from 0.4 to 52 MHz, so speed is not the cause.
 *
 * Milestone 3c (this build): SMU test. Mainline writes MPSCTRL0 = 0x71
 * (no SECURE_READ); Samsung's own bypass is 0xF1 (dw_mmc-exynos.h:212).
 *  - show EMMCP SMU registers as found, write Samsung's bypass, read back
 *  - re-run the read-only clock sweep and partition listing
 *
 * Milestone 3b (previous): diagnose the eMMC clock, strictly read-only.
 *  - read CMU_CCORE (Samsung S5E7880-cmusfr.h / -cmu.c / -pll.c) and show
 *    the real MMC0 clock S-Boot left: BUS_PLL/MEDIA_PLL /2, then DIV
 *  - sweep the card clock via debugfs (mmc0/clock), and at each step read
 *    the GPT header uncached; report pass/fail per frequency
 *  - keep the fastest passing clock, rescan partitions, list them
 *
 * Milestone 3 (previous build): eMMC, strictly read-only.
 *  - the whole eMMC is set read-only (BLKROSET) before anything is read
 *  - shows the card name, size and partition names, and checks the GPT
 *    header signature in LBA 1
 *  - keys stay live (re-test Home); console echo is off so key escape
 *    sequences no longer appear on screen
 */

#include <linux/fs.h>
#include <linux/input.h>
#include <linux/reboot.h>
#include <asm/termbits.h>

#define DECON_BASE		0x14830000UL	/* exynos7880.dtsi decon_f */
#define HW_SW_TRIG_CONTROL	0x70		/* decon_7880/regs-decon.h:92 */
#define TRIG_HW_CONTINUOUS	0x0010		/* HW_TRIG_EN, unmasked, no auto-mask */

#define HOLD_POWER_MS		3000
#define SAFETY_REBOOT_S		600
#define MAX_INPUTS		8

static void puts_(const char *s)
{
	write(1, s, strlen(s));
}

static void read_file(const char *path, char *buf, int len)
{
	int fd = open(path, O_RDONLY);
	int n = 0;

	buf[0] = 0;
	if (fd >= 0) {
		n = read(fd, buf, len - 1);
		close(fd);
	}
	if (n < 0)
		n = 0;
	buf[n] = 0;
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' '))
		buf[--n] = 0;
}

static void first_line_of(const char *path, const char *key, char *out, int len)
{
	static char buf[4096];
	char *p;

	read_file(path, buf, sizeof(buf));
	out[0] = 0;
	p = strstr(buf, key);
	if (!p)
		return;
	for (int i = 0; i < len - 1 && p[i] && p[i] != '\n'; i++) {
		out[i] = p[i];
		out[i + 1] = 0;
	}
}

/* Panel refresh: proven in milestone 1 */
static void panel_self_refresh(void)
{
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	volatile unsigned int *decon;

	if (fd < 0)
		return;
	decon = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, DECON_BASE);
	if (decon != MAP_FAILED)
		decon[HW_SW_TRIG_CONTROL / 4] = TRIG_HW_CONTINUOUS;
	close(fd);
}

static void reboot_recovery(const char *why)
{
	puts_("\n ");
	puts_(why);
	puts_(" -> rebooting into recovery (TWRP)...\n");
	msleep(1500);
	__nolibc_syscall4(__NR_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
			  LINUX_REBOOT_CMD_RESTART2, "recovery");
}

static const char *key_name(int code)
{
	switch (code) {
	case KEY_POWER:		return "POWER      ";
	case KEY_VOLUMEUP:	return "VOLUME UP  ";
	case KEY_VOLUMEDOWN:	return "VOLUME DOWN";
	case KEY_HOMEPAGE:	return "HOME       ";
	default:		return "other key  ";
	}
}

static void console_no_echo(void)
{
	struct termios t;

	if (ioctl(0, TCGETS, &t) == 0) {
		t.c_lflag &= ~(ECHO | ICANON);
		ioctl(0, TCSETS, &t);
	}
}

/* Wait up to ~6 s for the eMMC, make it read-only, then describe it. */
static void show_emmc(void)
{
	static char buf[512];
	char name[64], size[32], path[96], part[64];
	int fd, ro = 1, i, shown = 0;
	unsigned long long sectors;

	for (i = 0; i < 30 && (fd = open("/dev/mmcblk0", O_RDONLY)) < 0; i++)
		msleep(200);
	if (fd < 0) {
		puts_(" emmc   : NOT FOUND (mmcblk0 missing)\n");
		return;
	}
	if (ioctl(fd, BLKROSET, &ro) == 0)
		puts_(" emmc   : mmcblk0 set READ-ONLY\n");
	else
		puts_(" emmc   : BLKROSET failed (still only reading)\n");

	read_file("/sys/block/mmcblk0/device/name", name, sizeof(name));
	read_file("/sys/block/mmcblk0/size", size, sizeof(size));
	sectors = 0;
	for (i = 0; size[i] >= '0' && size[i] <= '9'; i++)
		sectors = sectors * 10 + (size[i] - '0');
	puts_(" emmc   : "); puts_(name); puts_(", ");
	puts_(utoa((unsigned int)(sectors / 2048))); puts_(" MiB\n");

	/* GPT header lives in LBA 1 and starts with "EFI PART" */
	if (lseek(fd, 512, SEEK_SET) == 512 && read(fd, buf, 512) == 512)
		puts_(memcmp(buf, "EFI PART", 8) == 0 ?
		      " emmc   : GPT header OK (\"EFI PART\" in LBA 1)\n" :
		      " emmc   : no GPT signature in LBA 1\n");
	else
		puts_(" emmc   : read of LBA 1 FAILED\n");
	close(fd);

	puts_(" parts  :");
	for (i = 1; i < 64; i++) {
		char *p;

		strcpy(path, "/sys/class/block/mmcblk0p");
		strlcat(path, utoa(i), sizeof(path));
		strlcat(path, "/uevent", sizeof(path));
		read_file(path, buf, sizeof(buf));
		p = strstr(buf, "PARTNAME=");
		if (!p)
			continue;
		p += 9;
		for (int k = 0; k < (int)sizeof(part) - 1; k++) {
			part[k] = (p[k] == '\n' || !p[k]) ? 0 : p[k];
			part[k + 1] = 0;
			if (!part[k])
				break;
		}
		if (shown && shown % 5 == 0)
			puts_("\n         ");
		puts_(" "); puts_(part);
		shown++;
	}
	puts_("\n parts  : ");
	puts_(utoa(shown));
	puts_(" partitions\n");
}

static unsigned int rd32(volatile unsigned int *base, unsigned int off)
{
	return base[off / 4];
}

static void puthex(unsigned int v)
{
	char b[11] = "0x00000000";

	for (int i = 0; i < 8; i++)
		b[9 - i] = "0123456789abcdef"[(v >> (i * 4)) & 0xf];
	puts_(b);
}

static void putmhz(unsigned long long hz)
{
	puts_(utoa((unsigned int)(hz / 1000000)));
	puts_(".");
	puts_(utoa((unsigned int)((hz / 100000) % 10)));
	puts_(" MHz");
}

/* f = 26 MHz * M / (P << S), Samsung S5E7880-pll.c pll141xx */
static unsigned long long pll141xx(unsigned int con0)
{
	unsigned int m = (con0 >> 16) & 0x3ff, p = (con0 >> 8) & 0x3f, s = con0 & 7;

	if (!(con0 & (1u << 31)) || !p)
		return 0;
	return 26000000ULL * m / ((unsigned long long)p << s);
}

static void show_mmc_clock(void)
{
	int fd = open("/dev/mem", O_RDONLY | O_SYNC);
	volatile unsigned int *cc;
	unsigned int media, bus, mux, div;
	unsigned long long parent, rate;

	if (fd < 0)
		return;
	cc = mmap(NULL, 0x1000, PROT_READ, MAP_SHARED, fd, 0x10680000);	/* CMU_CCORE */
	close(fd);
	if (cc == MAP_FAILED) {
		puts_(" clk    : CMU_CCORE map failed\n");
		return;
	}
	media = rd32(cc, 0x120);	/* MEDIA_PLL_CON0 */
	bus = rd32(cc, 0x140);		/* BUS_PLL_CON0 */
	mux = rd32(cc, 0x288);		/* CLK_CON_MUX_CLKCMU_FSYS_MMC0, bit 12 */
	div = rd32(cc, 0x488);		/* CLK_CON_DIV_CLKCMU_FSYS_MMC0, bits 0-9 */

	puts_(" clk    : BUS_PLL "); puthex(bus); puts_(" = "); putmhz(pll141xx(bus)); puts_("\n");
	puts_(" clk    : MEDIA_PLL "); puthex(media); puts_(" = "); putmhz(pll141xx(media)); puts_("\n");
	puts_(" clk    : MMC0 mux "); puthex(mux); puts_(" div "); puthex(div); puts_("\n");
	parent = ((mux >> 12) & 1) ? pll141xx(media) / 2 : pll141xx(bus) / 2;
	rate = parent / ((div & 0x3ff) + 1);
	puts_(" clk    : MMC0 CIU = "); putmhz(rate);
	puts_((mux >> 12) & 1 ? " (MEDIA_PLL/2" : " (BUS_PLL/2");
	puts_(" / "); puts_(utoa((div & 0x3ff) + 1)); puts_(")\n");
}

/* EMMCP (SMU) at MMC0 + 0x1000: Samsung dw_mmc-exynos.h:122-130 */
#define EMMCP_BASE	0x13541000UL
#define MPSECURITY	0x010
#define MPSBEGIN0	0x200
#define MPSEND0		0x204
#define MPSCTRL0	0x20c
#define MPSCTRL_SAMSUNG_BYPASS	0xf1	/* SECURE_R|SECURE_W|NS_R|NS_W|VALID */

static void show_smu(volatile unsigned int *e, const char *when)
{
	puts_(" smu    : "); puts_(when);
	puts_(" SEC "); puthex(rd32(e, MPSECURITY));
	puts_(" CTRL0 "); puthex(rd32(e, MPSCTRL0));
	puts_("\n          BEGIN0 "); puthex(rd32(e, MPSBEGIN0));
	puts_(" END0 "); puthex(rd32(e, MPSEND0)); puts_("\n");
}

static void smu_samsung_bypass(void)
{
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	volatile unsigned int *e;

	if (fd < 0)
		return;
	e = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, EMMCP_BASE);
	close(fd);
	if (e == MAP_FAILED) {
		puts_(" smu    : map failed\n");
		return;
	}
	show_smu(e, "found ");
	e[MPSBEGIN0 / 4] = 0;
	e[MPSEND0 / 4] = 0xffffffff;
	e[MPSCTRL0 / 4] = MPSCTRL_SAMSUNG_BYPASS;
	show_smu(e, "bypass");
}

static void write_str(const char *path, const char *v)
{
	int fd = open(path, O_WRONLY);

	if (fd >= 0) {
		write(fd, v, strlen(v));
		close(fd);
	}
}

/* Read the GPT header (LBA 1) and 64 KiB from the start, bypassing caches */
static int emmc_read_ok(void)
{
	static char buf[65536];
	int fd = open("/dev/mmcblk0", O_RDONLY), ok;

	if (fd < 0)
		return 0;
	ioctl(fd, BLKFLSBUF, 0);
	ok = lseek(fd, 0, SEEK_SET) == 0 &&
	     read(fd, buf, sizeof(buf)) == (ssize_t)sizeof(buf) &&
	     memcmp(buf + 512, "EFI PART", 8) == 0;
	close(fd);
	return ok;
}

static void emmc_clock_sweep(void)
{
	static const unsigned int f[] = {
		400000, 1000000, 5000000, 10000000, 20000000,
		26000000, 40000000, 52000000,
	};
	unsigned int best = 0;
	int fd, ro = 1;

	for (int i = 0; i < 30 && (fd = open("/dev/mmcblk0", O_RDONLY)) < 0; i++)
		msleep(200);
	if (fd < 0) {
		puts_(" sweep  : mmcblk0 missing\n");
		return;
	}
	ioctl(fd, BLKROSET, &ro);
	close(fd);

	mkdir("/sys/kernel/debug", 0755);
	mount("debugfs", "/sys/kernel/debug", "debugfs", 0, NULL);
	write_str("/proc/sys/kernel/printk", "1");	/* keep I/O errors off screen */

	puts_(" sweep  :");
	for (unsigned int i = 0; i < sizeof(f) / sizeof(f[0]); i++) {
		int ok;

		write_str("/sys/kernel/debug/mmc0/clock", utoa(f[i]));
		msleep(50);
		ok = emmc_read_ok() && emmc_read_ok();
		puts_(" ");
		puts_(utoa(f[i] / 100000 / 10)); puts_(".");
		puts_(utoa(f[i] / 100000 % 10));
		puts_(ok ? ":OK" : ":FAIL");
		if (ok)
			best = f[i];
	}
	puts_(" (MHz)\n");

	if (best) {
		write_str("/sys/kernel/debug/mmc0/clock", utoa(best));
		fd = open("/dev/mmcblk0", O_RDONLY);
		if (fd >= 0) {
			ioctl(fd, BLKRRPART, 0);
			close(fd);
		}
		msleep(300);
		puts_(" sweep  : using "); putmhz(best); puts_(", partitions rescanned\n");
	} else {
		puts_(" sweep  : no frequency passed\n");
	}
	write_str("/proc/sys/kernel/printk", "7");
}

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main(void)
{
	struct pollfd fds[MAX_INPUTS];
	struct utsname u;
	char line[160], cpus[64], name[128], path[64];
	int nfds = 0, presses = 0;
	long long power_down_at = -1, start;

	mkdir("/proc", 0755);
	mkdir("/sys", 0755);
	mkdir("/dev", 0755);
	mount("proc", "/proc", "proc", 0, NULL);
	mount("sysfs", "/sys", "sysfs", 0, NULL);
	mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);

	panel_self_refresh();

	uname(&u);
	read_file("/sys/devices/system/cpu/online", cpus, sizeof(cpus));
	first_line_of("/proc/meminfo", "MemTotal", line, sizeof(line));

	console_no_echo();

	puts_("\n\n=========================================\n");
	puts_(" Galaxy A5 (2017) - mainline Linux M3c\n");
	puts_("=========================================\n");
	puts_(" kernel : "); puts_(u.release); puts_(" "); puts_(u.machine); puts_("\n");
	puts_(" cpus   : "); puts_(cpus); puts_("\n");
	puts_(" memory : "); puts_(line); puts_("\n\n");

	/* Open every input device and show its name */
	for (int i = 0; i < MAX_INPUTS; i++) {
		int fd;

		strcpy(path, "/dev/input/event");
		strlcat(path, utoa(i), sizeof(path));
		fd = open(path, O_RDONLY | O_NONBLOCK);
		if (fd < 0)
			continue;
		strcpy(path, "/sys/class/input/event");
		strlcat(path, utoa(i), sizeof(path));
		strlcat(path, "/device/name", sizeof(path));
		read_file(path, name, sizeof(name));
		puts_(" input  : event"); puts_(utoa(i)); puts_(" = "); puts_(name); puts_("\n");
		fds[nfds].fd = fd;
		fds[nfds].events = POLLIN;
		nfds++;
	}
	if (!nfds)
		puts_(" input  : NONE FOUND (gpio-keys did not probe)\n");

	write_str("/proc/sys/kernel/printk", "1");
	show_mmc_clock();
	smu_samsung_bypass();
	emmc_clock_sweep();
	show_emmc();
	write_str("/proc/sys/kernel/printk", "7");

	puts_("\n Press Power, Volume Up, Volume Down, Home.\n");
	puts_(" Hold POWER for 3 s to reboot into TWRP.\n\n");

	start = now_ms();
	for (;;) {
		int r = poll(fds, nfds, 200);

		if (now_ms() - start > SAFETY_REBOOT_S * 1000LL)
			reboot_recovery("safety timeout");
		if (power_down_at >= 0 && now_ms() - power_down_at >= HOLD_POWER_MS)
			reboot_recovery("POWER held 3 s");
		if (r <= 0)
			continue;

		for (int i = 0; i < nfds; i++) {
			struct input_event ev;

			if (!(fds[i].revents & POLLIN))
				continue;
			while (read(fds[i].fd, &ev, sizeof(ev)) == sizeof(ev)) {
				if (ev.type != EV_KEY || ev.value == 2)
					continue;
				if (ev.value)
					presses++;
				puts_(" ");
				puts_(key_name(ev.code));
				puts_(ev.value ? "  pressed " : "  released");
				puts_("   (code ");
				puts_(utoa(ev.code));
				puts_(", total presses ");
				puts_(utoa(presses));
				puts_(")\n");
				if (ev.code == KEY_POWER)
					power_down_at = ev.value ? now_ms() : -1;
			}
		}
	}
	return 0;
}
