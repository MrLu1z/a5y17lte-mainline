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
 * Milestone 2 (this build): show the four hardware keys live.
 *  - every press/release of Power, Volume Up/Down and Home is printed
 *  - holding Power for 3 s reboots into recovery (TWRP)
 *  - safety net: reboot into recovery after SAFETY_REBOOT_S anyway
 */

#include <linux/input.h>
#include <linux/reboot.h>

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

	puts_("\n\n=========================================\n");
	puts_(" Galaxy A5 (2017) - mainline Linux M2\n");
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
