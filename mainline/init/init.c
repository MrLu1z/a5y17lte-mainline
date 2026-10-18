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
 * Milestone 3 (this build): eMMC, strictly read-only.
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
	puts_(" Galaxy A5 (2017) - mainline Linux M3\n");
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

	show_emmc();

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
